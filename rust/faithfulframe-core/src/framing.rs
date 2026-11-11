//! Pure normalized crop planning with bounded pan and zoom smoothing.

use crate::Rect;

#[derive(Clone, Copy, Debug)]
pub struct FramingConfig {
    pub source_aspect: f32,
    pub output_aspect: f32,
    pub target_coverage: f32,
    pub min_zoom: f32,
    pub max_zoom: f32,
    pub smoothing: f32,
    /// Fraction inset from each edge of the current crop. Pan holds while the
    /// complete target rectangle stays inside this inner crop region.
    pub dead_zone_fraction: f32,
    pub max_pan_step: f32,
    pub max_zoom_step: f32,
}

impl Default for FramingConfig {
    fn default() -> Self {
        Self {
            source_aspect: 16.0 / 9.0,
            output_aspect: 16.0 / 9.0,
            target_coverage: 0.45,
            min_zoom: 1.0,
            max_zoom: 4.0,
            smoothing: 0.25,
            dead_zone_fraction: 0.05,
            max_pan_step: 0.08,
            max_zoom_step: 0.08,
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum FramingError {
    InvalidConfig,
    InvalidTarget,
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct FramePlan {
    /// Normalized source-space rectangle sampled from the camera.
    pub source_crop: Rect,
    /// Normalized output-space viewport. A smaller viewport adds padding so
    /// safe-wide framing preserves the complete source image.
    pub output_viewport: Rect,
}

/// The controller owns no clocks, threads, or external resources. The caller
/// invokes `plan` once per authorized target update on the session executor.
pub struct FramingController {
    config: FramingConfig,
    current: Option<Rect>,
}

impl FramingController {
    pub fn new(config: FramingConfig) -> Result<Self, FramingError> {
        let crop_aspect = config.output_aspect / config.source_aspect;
        if !config.output_aspect.is_finite()
            || !config.source_aspect.is_finite()
            || !crop_aspect.is_finite()
            || !config.target_coverage.is_finite()
            || !config.min_zoom.is_finite()
            || !config.max_zoom.is_finite()
            || !config.smoothing.is_finite()
            || !config.dead_zone_fraction.is_finite()
            || !config.max_pan_step.is_finite()
            || !config.max_zoom_step.is_finite()
            || config.output_aspect <= 0.0
            || config.source_aspect <= 0.0
            || crop_aspect <= 0.0
            || !(0.0 < config.target_coverage && config.target_coverage <= 1.0)
            || config.min_zoom < 1.0
            || config.max_zoom < config.min_zoom
            || !(0.0 < config.smoothing && config.smoothing <= 1.0)
            || !(0.0..0.5).contains(&config.dead_zone_fraction)
            || config.max_pan_step <= 0.0
            || config.max_zoom_step <= 0.0
        {
            return Err(FramingError::InvalidConfig);
        }
        Ok(Self {
            config,
            current: None,
        })
    }

    pub fn reset(&mut self) {
        self.current = None;
    }

    pub fn safe_wide(&self) -> FramePlan {
        FramePlan {
            source_crop: Rect::full_frame(),
            output_viewport: fit_viewport(self.config.source_aspect, self.config.output_aspect),
        }
    }

    /// Returns an aspect-correct source crop containing target, or full source
    /// with fit padding when the target cannot fit the configured zoom limits.
    pub fn plan(&mut self, target: Rect) -> Result<FramePlan, FramingError> {
        Rect::normalized(target.x, target.y, target.width, target.height)
            .map_err(|_| FramingError::InvalidTarget)?;
        let desired = self.desired(target);
        let next = match self.current {
            None => desired,
            Some(_) if desired == Rect::full_frame() => desired,
            Some(current) if !self.has_output_aspect(current) => desired,
            Some(current) => self.smooth(current, desired, target),
        };
        if next != Rect::full_frame() && !self.has_output_aspect(next) {
            return Err(FramingError::InvalidTarget);
        }
        self.current = Some(next);
        Ok(self.plan_for(next))
    }

    fn desired(&self, target: Rect) -> Rect {
        let crop_aspect = self.crop_aspect();
        let base_width = crop_aspect.min(1.0);
        let base_height = base_width / crop_aspect;
        let max_fitting_zoom = (base_width * self.config.target_coverage / target.width)
            .min(base_height * self.config.target_coverage / target.height);
        if max_fitting_zoom < self.config.min_zoom {
            return Rect::full_frame();
        }
        let zoom = max_fitting_zoom.min(self.config.max_zoom);
        bounded_centered(target, base_width / zoom, base_height / zoom)
    }

    fn smooth(&self, current: Rect, desired: Rect, target: Rect) -> Rect {
        if desired == Rect::full_frame() {
            return desired;
        }
        let crop_aspect = self.crop_aspect();
        let current_center = (
            current.x + current.width * 0.5,
            current.y + current.height * 0.5,
        );
        let max_pan_delta = self.config.max_pan_step * self.config.smoothing;
        let target_inside_dead_zone = self.config.dead_zone_fraction > 0.0
            && current != Rect::full_frame()
            && target.x >= current.x + current.width * self.config.dead_zone_fraction
            && target.x + target.width
                <= current.x + current.width * (1.0 - self.config.dead_zone_fraction)
            && target.y >= current.y + current.height * self.config.dead_zone_fraction
            && target.y + target.height
                <= current.y + current.height * (1.0 - self.config.dead_zone_fraction);
        let desired_center = (
            desired.x + desired.width * 0.5,
            desired.y + desired.height * 0.5,
        );
        let (mut center_x, mut center_y) = if target_inside_dead_zone {
            current_center
        } else {
            (
                current_center.0
                    + bounded_delta(
                        desired_center.0 - current_center.0,
                        self.config.max_pan_step,
                    ) * self.config.smoothing,
                current_center.1
                    + bounded_delta(
                        desired_center.1 - current_center.1,
                        self.config.max_pan_step,
                    ) * self.config.smoothing,
            )
        };
        let mut width = current.width
            + bounded_delta(desired.width - current.width, self.config.max_zoom_step)
                * self.config.smoothing;
        // Keep pan within its configured step. If that center cannot yet
        // contain the target, widen temporarily instead of teleporting the
        // crop across the source.
        let max_crop_width = crop_aspect.min(1.0);
        for _ in 0..3 {
            center_x = center_x.clamp(width * 0.5, 1.0 - width * 0.5);
            let height = width / crop_aspect;
            center_y = center_y.clamp(height * 0.5, 1.0 - height * 0.5);
            if (center_x - current_center.0).abs() > max_pan_delta
                || (center_y - current_center.1).abs() > max_pan_delta
            {
                return Rect::full_frame();
            }
            let needed_width =
                (2.0 * (target.x + target.width * 0.5 - center_x).abs() + target.width).max(
                    (2.0 * (target.y + target.height * 0.5 - center_y).abs() + target.height)
                        * crop_aspect,
                );
            if needed_width <= width {
                return bounded_at(center_x, center_y, width, height);
            }
            width = needed_width;
            if width > max_crop_width {
                return Rect::full_frame();
            }
        }
        Rect::full_frame()
    }

    fn crop_aspect(&self) -> f32 {
        self.config.output_aspect / self.config.source_aspect
    }

    fn has_output_aspect(&self, crop: Rect) -> bool {
        (crop.width / crop.height - self.crop_aspect()).abs() <= 0.001
    }

    fn plan_for(&self, crop: Rect) -> FramePlan {
        let output_viewport = if crop == Rect::full_frame() {
            fit_viewport(self.config.source_aspect, self.config.output_aspect)
        } else {
            Rect::full_frame()
        };
        FramePlan {
            source_crop: crop,
            output_viewport,
        }
    }
}

fn bounded_delta(value: f32, limit: f32) -> f32 {
    value.clamp(-limit, limit)
}

fn bounded_centered(target: Rect, width: f32, height: f32) -> Rect {
    bounded_at(
        target.x + target.width * 0.5,
        target.y + target.height * 0.5,
        width,
        height,
    )
}

fn bounded_at(center_x: f32, center_y: f32, width: f32, height: f32) -> Rect {
    let width = width.clamp(0.000_001, 1.0);
    let height = height.clamp(0.000_001, 1.0);
    let x = (center_x - width * 0.5).clamp(0.0, 1.0 - width);
    let y = (center_y - height * 0.5).clamp(0.0, 1.0 - height);
    Rect::normalized(x, y, width, height).expect("bounded crop must be valid")
}

#[cfg(test)]
fn contains(outer: Rect, inner: Rect) -> bool {
    outer.x <= inner.x
        && outer.y <= inner.y
        && outer.x + outer.width >= inner.x + inner.width
        && outer.y + outer.height >= inner.y + inner.height
}

fn fit_viewport(source_aspect: f32, output_aspect: f32) -> Rect {
    if source_aspect > output_aspect {
        let height = output_aspect / source_aspect;
        Rect {
            x: 0.0,
            y: (1.0 - height) * 0.5,
            width: 1.0,
            height,
        }
    } else if source_aspect < output_aspect {
        let width = source_aspect / output_aspect;
        Rect {
            x: (1.0 - width) * 0.5,
            y: 0.0,
            width,
            height: 1.0,
        }
    } else {
        Rect::full_frame()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn target(x: f32, y: f32, width: f32, height: f32) -> Rect {
        Rect::normalized(x, y, width, height).unwrap()
    }

    #[test]
    fn crop_is_bounded_aspect_correct_and_contains_target() {
        let mut controller = FramingController::new(FramingConfig::default()).unwrap();
        let speaker = target(0.4, 0.3, 0.1, 0.25);
        let plan = controller.plan(speaker).unwrap();
        let crop = plan.source_crop;
        assert!(contains(crop, speaker));
        // Normalized source coordinates scale x by source width and y by
        // source height. Equal source/output aspect therefore means a 1:1
        // normalized crop, even though its pixel aspect is 16:9.
        assert!((crop.width / crop.height - 1.0).abs() < 0.000_1);
        assert_eq!(plan.output_viewport, Rect::full_frame());
        assert!(crop.x >= 0.0 && crop.y >= 0.0);
        assert!(crop.x + crop.width <= 1.0 && crop.y + crop.height <= 1.0);
    }

    #[test]
    fn edge_target_is_kept_in_bounds() {
        let mut controller = FramingController::new(FramingConfig::default()).unwrap();
        let crop = controller
            .plan(target(0.92, 0.7, 0.08, 0.2))
            .unwrap()
            .source_crop;
        assert!(crop.x + crop.width <= 1.0);
        assert!(crop.y + crop.height <= 1.0);
    }

    #[test]
    fn oversized_target_falls_back_to_full_source() {
        let mut controller = FramingController::new(FramingConfig::default()).unwrap();
        let plan = controller.plan(target(0.0, 0.0, 1.0, 1.0)).unwrap();
        assert_eq!(plan.source_crop, Rect::full_frame());
        assert_eq!(plan.output_viewport, Rect::full_frame());
    }

    #[test]
    fn safe_wide_fits_complete_source_with_padding() {
        let controller = FramingController::new(FramingConfig {
            source_aspect: 4.0 / 3.0,
            output_aspect: 16.0 / 9.0,
            ..FramingConfig::default()
        })
        .unwrap();
        let plan = controller.safe_wide();
        assert_eq!(plan.source_crop, Rect::full_frame());
        assert!((plan.output_viewport.width - 0.75).abs() < 0.000_1);
        assert_eq!(plan.output_viewport.x, 0.125);
        assert_eq!(plan.output_viewport.y, 0.0);
        assert_eq!(plan.output_viewport.height, 1.0);
    }

    #[test]
    fn aspect_conversion_matches_pixel_aspect_for_4k_to_1080p() {
        let mut controller = FramingController::new(FramingConfig {
            source_aspect: 3840.0 / 2160.0,
            output_aspect: 1920.0 / 1080.0,
            ..FramingConfig::default()
        })
        .unwrap();
        let crop = controller
            .plan(target(0.4, 0.3, 0.1, 0.2))
            .unwrap()
            .source_crop;
        assert!((crop.width / crop.height - 1.0).abs() < 0.000_1);
        assert!(contains(crop, target(0.4, 0.3, 0.1, 0.2)));
    }

    #[test]
    fn input_1080p_to_1080p_uses_resolution_independent_geometry() {
        let mut controller = FramingController::new(FramingConfig {
            source_aspect: 1920.0 / 1080.0,
            output_aspect: 1920.0 / 1080.0,
            ..FramingConfig::default()
        })
        .unwrap();
        let target = target(0.4, 0.3, 0.1, 0.2);
        let plan = controller.plan(target).unwrap();
        assert!(contains(plan.source_crop, target));
        assert!((plan.source_crop.width / plan.source_crop.height - 1.0).abs() < 0.000_1);
    }

    #[test]
    fn maximum_zoom_is_respected_for_small_targets() {
        let mut controller = FramingController::new(FramingConfig {
            target_coverage: 0.5,
            max_zoom: 2.0,
            ..FramingConfig::default()
        })
        .unwrap();
        let plan = controller.plan(target(0.49, 0.49, 0.01, 0.01)).unwrap();
        assert!((plan.source_crop.width - 0.5).abs() < 0.000_1);
        assert!((plan.source_crop.height - 0.5).abs() < 0.000_1);
    }

    #[test]
    fn portrait_output_uses_wider_normalized_source_crop() {
        let mut controller = FramingController::new(FramingConfig {
            source_aspect: 16.0 / 9.0,
            output_aspect: 9.0 / 16.0,
            ..FramingConfig::default()
        })
        .unwrap();
        let plan = controller.plan(target(0.4, 0.3, 0.08, 0.16)).unwrap();
        assert!((plan.source_crop.width / plan.source_crop.height - 81.0 / 256.0).abs() < 0.000_1);
    }

    #[test]
    fn smoothing_preserves_aspect_and_target_containment() {
        let mut controller = FramingController::new(FramingConfig {
            smoothing: 0.5,
            max_pan_step: 0.2,
            max_zoom_step: 0.2,
            ..FramingConfig::default()
        })
        .unwrap();
        let first = controller.plan(target(0.4, 0.3, 0.08, 0.16)).unwrap();
        let previous_center = (
            first.source_crop.x + first.source_crop.width * 0.5,
            first.source_crop.y + first.source_crop.height * 0.5,
        );
        let target = target(0.45, 0.3, 0.08, 0.16);
        let plan = controller.plan(target).unwrap();
        assert!(contains(plan.source_crop, target));
        assert!((plan.source_crop.width / plan.source_crop.height - 1.0).abs() < 0.000_1);
        let center = (
            plan.source_crop.x + plan.source_crop.width * 0.5,
            plan.source_crop.y + plan.source_crop.height * 0.5,
        );
        assert!((center.0 - previous_center.0).abs() <= 0.1);
        assert!((center.1 - previous_center.1).abs() <= 0.1);
    }

    #[test]
    fn dead_zone_holds_pan_until_target_leaves_inner_crop_region() {
        let mut controller = FramingController::new(FramingConfig {
            dead_zone_fraction: 0.1,
            smoothing: 0.5,
            max_pan_step: 0.2,
            ..FramingConfig::default()
        })
        .unwrap();
        let first_target = target(0.46, 0.46, 0.08, 0.08);
        let first = controller.plan(first_target).unwrap();
        let held = controller.plan(target(0.49, 0.46, 0.08, 0.08)).unwrap();
        assert_eq!(held.source_crop, first.source_crop);

        let moving_target = target(0.57, 0.46, 0.08, 0.08);
        let moving = controller.plan(moving_target).unwrap();
        let first_center_x = first.source_crop.x + first.source_crop.width * 0.5;
        let moving_center_x = moving.source_crop.x + moving.source_crop.width * 0.5;
        assert!(moving_center_x > first_center_x);
        assert!(moving_center_x - first_center_x <= 0.1);
        assert!(contains(moving.source_crop, moving_target));
    }

    #[test]
    fn dead_zone_allows_zoom_to_adjust_without_pan() {
        let mut controller = FramingController::new(FramingConfig {
            dead_zone_fraction: 0.1,
            smoothing: 0.5,
            max_zoom: 8.0,
            max_zoom_step: 0.2,
            ..FramingConfig::default()
        })
        .unwrap();
        let first = controller.plan(target(0.46, 0.46, 0.08, 0.08)).unwrap();
        let narrower_target = target(0.48, 0.48, 0.04, 0.04);
        let next = controller.plan(narrower_target).unwrap();
        let first_center = (
            first.source_crop.x + first.source_crop.width * 0.5,
            first.source_crop.y + first.source_crop.height * 0.5,
        );
        let next_center = (
            next.source_crop.x + next.source_crop.width * 0.5,
            next.source_crop.y + next.source_crop.height * 0.5,
        );

        assert_eq!(next_center, first_center);
        assert!(next.source_crop.width < first.source_crop.width);
        assert!(contains(next.source_crop, narrower_target));
    }

    #[test]
    fn large_pan_that_breaks_step_limit_falls_back_to_safe_wide() {
        let mut controller = FramingController::new(FramingConfig::default()).unwrap();
        controller.plan(target(0.2, 0.3, 0.08, 0.16)).unwrap();
        let plan = controller.plan(target(0.9, 0.3, 0.08, 0.16)).unwrap();
        assert_eq!(plan.source_crop, Rect::full_frame());
        assert_eq!(plan.output_viewport, Rect::full_frame());
    }

    #[test]
    fn invalid_configuration_is_rejected() {
        assert!(matches!(
            FramingController::new(FramingConfig {
                output_aspect: 0.0,
                ..FramingConfig::default()
            }),
            Err(FramingError::InvalidConfig)
        ));
        assert!(matches!(
            FramingController::new(FramingConfig {
                source_aspect: f32::NAN,
                ..FramingConfig::default()
            }),
            Err(FramingError::InvalidConfig)
        ));
        assert!(matches!(
            FramingController::new(FramingConfig {
                dead_zone_fraction: 0.5,
                ..FramingConfig::default()
            }),
            Err(FramingError::InvalidConfig)
        ));
    }
}
