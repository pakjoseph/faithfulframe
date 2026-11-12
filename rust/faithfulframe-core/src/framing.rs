//! Pure normalized crop planning with bounded pan and zoom smoothing.

use crate::Rect;

const NOMINAL_SAMPLE_PERIOD_SECONDS: f32 = 1.0 / 30.0;
const MAX_INTEGRATION_STEP_SECONDS: f32 = 0.1;
const FULL_SPEED_RAMP_SECONDS: f32 = 0.5;

#[derive(Clone, Copy, Debug)]
pub struct FramingConfig {
    pub source_aspect: f32,
    pub output_aspect: f32,
    pub target_coverage: f32,
    pub min_zoom: f32,
    pub max_zoom: f32,
    pub pan_smoothing: f32,
    pub zoom_smoothing: f32,
    /// Fraction inset from each edge of the current crop. Pan holds while the
    /// complete target rectangle stays inside this inner crop region.
    pub dead_zone_fraction: f32,
    /// Maximum normalized pan displacement per nominal 30 Hz update.
    pub max_pan_step: f32,
    /// Maximum normalized crop-extent change per nominal 30 Hz update.
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
            pan_smoothing: 0.25,
            zoom_smoothing: 0.25,
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
    NonMonotonicTime,
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
/// invokes `plan_at` once per authorized target update on the session executor.
pub struct FramingController {
    config: FramingConfig,
    current: Option<Rect>,
    last_plan_at_ms: Option<u64>,
    pan_velocity: (f32, f32),
    zoom_velocity: (f32, f32),
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
            || !config.pan_smoothing.is_finite()
            || !config.zoom_smoothing.is_finite()
            || !config.dead_zone_fraction.is_finite()
            || !config.max_pan_step.is_finite()
            || !config.max_zoom_step.is_finite()
            || config.output_aspect <= 0.0
            || config.source_aspect <= 0.0
            || crop_aspect <= 0.0
            || !(0.0 < config.target_coverage && config.target_coverage <= 1.0)
            || config.min_zoom < 1.0
            || config.max_zoom < config.min_zoom
            || !(0.0 < config.pan_smoothing && config.pan_smoothing <= 1.0)
            || !(0.0 < config.zoom_smoothing && config.zoom_smoothing <= 1.0)
            || !(0.0..0.5).contains(&config.dead_zone_fraction)
            || config.max_pan_step <= 0.0
            || config.max_pan_step > 1.0
            || config.max_zoom_step <= 0.0
            || config.max_zoom_step > 1.0
        {
            return Err(FramingError::InvalidConfig);
        }
        let wide_viewport = fit_viewport(config.source_aspect, config.output_aspect);
        if Rect::normalized(
            wide_viewport.x,
            wide_viewport.y,
            wide_viewport.width,
            wide_viewport.height,
        )
        .is_err()
        {
            return Err(FramingError::InvalidConfig);
        }
        Ok(Self {
            config,
            current: None,
            last_plan_at_ms: None,
            pan_velocity: (0.0, 0.0),
            zoom_velocity: (0.0, 0.0),
        })
    }

    pub fn reset(&mut self) {
        self.current = None;
        self.reset_motion();
    }

    /// Holds the current crop and clears motion after a pause, target loss, or
    /// operator action. A later lock starts with zero residual velocity.
    pub fn hold(&mut self) {
        self.reset_motion();
    }

    fn reset_motion(&mut self) {
        self.last_plan_at_ms = None;
        self.pan_velocity = (0.0, 0.0);
        self.zoom_velocity = (0.0, 0.0);
    }

    pub fn safe_wide(&self) -> FramePlan {
        FramePlan {
            source_crop: Rect::full_frame(),
            output_viewport: fit_viewport(self.config.source_aspect, self.config.output_aspect),
        }
    }

    /// Returns a bounded source crop and an output viewport that preserves its
    /// aspect ratio. This convenience method advances a deterministic nominal
    /// 30 Hz step; production callers should use `plan_at` with event time.
    pub fn plan(&mut self, target: Rect) -> Result<FramePlan, FramingError> {
        let at = self
            .last_plan_at_ms
            .map_or(0, |previous| previous.saturating_add(33));
        self.plan_at(target, at)
    }

    /// Advances the framing controller using monotonic milliseconds. Pan and
    /// zoom acceleration are bounded in source-normalized units per second²;
    /// integration time is capped so a long pause cannot cause a catch-up jump.
    pub fn plan_at(&mut self, target: Rect, at_ms: u64) -> Result<FramePlan, FramingError> {
        Rect::normalized(target.x, target.y, target.width, target.height)
            .map_err(|_| FramingError::InvalidTarget)?;

        if self
            .last_plan_at_ms
            .is_some_and(|previous| at_ms < previous)
        {
            return Err(FramingError::NonMonotonicTime);
        }
        if self.last_plan_at_ms == Some(at_ms) {
            return self.plan_for(self.current.unwrap_or_else(Rect::full_frame));
        }

        let dt = self
            .last_plan_at_ms
            .map_or(NOMINAL_SAMPLE_PERIOD_SECONDS, |previous| {
                ((at_ms - previous) as f32 / 1_000.0).min(MAX_INTEGRATION_STEP_SECONDS)
            });
        self.last_plan_at_ms = Some(at_ms);
        let desired = self.desired(target);
        // The first selection begins at safe-wide too. Intermediate crop
        // rectangles may have a different aspect ratio from the output; fit
        // them with padding until pan and zoom converge to the target crop.
        let current = self.current.unwrap_or_else(Rect::full_frame);
        let next = self.smooth(current, desired, target, dt);
        Rect::normalized(next.x, next.y, next.width, next.height)
            .map_err(|_| FramingError::InvalidTarget)?;
        let plan = self.plan_for(next)?;
        self.current = Some(next);
        Ok(plan)
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

    fn smooth(&mut self, current: Rect, desired: Rect, target: Rect, dt: f32) -> Rect {
        if desired == Rect::full_frame() {
            self.pan_velocity = (0.0, 0.0);
            self.zoom_velocity = (0.0, 0.0);
            return desired;
        }
        let current_center = (
            current.x + current.width * 0.5,
            current.y + current.height * 0.5,
        );
        let max_pan_delta = self.config.max_pan_step;
        let target_inside_x_dead_zone = self.config.dead_zone_fraction > 0.0
            && current != Rect::full_frame()
            && target.x >= current.x + current.width * self.config.dead_zone_fraction
            && target.x + target.width
                <= current.x + current.width * (1.0 - self.config.dead_zone_fraction);
        let target_inside_y_dead_zone = self.config.dead_zone_fraction > 0.0
            && current != Rect::full_frame()
            && target.y >= current.y + current.height * self.config.dead_zone_fraction
            && target.y + target.height
                <= current.y + current.height * (1.0 - self.config.dead_zone_fraction);
        let desired_center = (
            desired.x + desired.width * 0.5,
            desired.y + desired.height * 0.5,
        );
        let max_pan_velocity = self.config.max_pan_step / NOMINAL_SAMPLE_PERIOD_SECONDS;
        let max_zoom_velocity = self.config.max_zoom_step / NOMINAL_SAMPLE_PERIOD_SECONDS;
        // Ramp to each configured speed cap over half a second. `max_*_step`
        // continues to bound displacement per update as an additional guard.
        let max_pan_acceleration = max_pan_velocity / FULL_SPEED_RAMP_SECONDS;
        let max_zoom_acceleration = max_zoom_velocity / FULL_SPEED_RAMP_SECONDS;
        let requested_x = if target_inside_x_dead_zone {
            current_center.0
        } else {
            desired_center.0
        };
        let requested_y = if target_inside_y_dead_zone {
            current_center.1
        } else {
            desired_center.1
        };
        let (pan_x, velocity_x) = advance_axis(
            current_center.0,
            requested_x,
            self.pan_velocity.0,
            self.config.max_pan_step,
            max_pan_velocity,
            max_pan_acceleration,
            self.config.pan_smoothing,
            dt,
        );
        let (pan_y, velocity_y) = advance_axis(
            current_center.1,
            requested_y,
            self.pan_velocity.1,
            self.config.max_pan_step,
            max_pan_velocity,
            max_pan_acceleration,
            self.config.pan_smoothing,
            dt,
        );
        let (next_width, velocity_width) = advance_axis(
            current.width,
            desired.width,
            self.zoom_velocity.0,
            self.config.max_zoom_step,
            max_zoom_velocity,
            max_zoom_acceleration,
            self.config.zoom_smoothing,
            dt,
        );
        let (next_height, velocity_height) = advance_axis(
            current.height,
            desired.height,
            self.zoom_velocity.1,
            self.config.max_zoom_step,
            max_zoom_velocity,
            max_zoom_acceleration,
            self.config.zoom_smoothing,
            dt,
        );
        self.pan_velocity = (velocity_x, velocity_y);
        self.zoom_velocity = (velocity_width, velocity_height);
        let mut center_x = pan_x;
        let mut center_y = pan_y;
        let width = next_width;
        let height = next_height;
        let target_center_x = target.x + target.width * 0.5;
        let target_center_y = target.y + target.height * 0.5;

        // Keep pan and zoom within their acceleration limits. If the planned
        // movement cannot contain the target yet, use safe-wide instead of
        // widening abruptly and bypassing the zoom ramp.
        if width > 1.0 || height > 1.0 {
            self.pan_velocity = (0.0, 0.0);
            self.zoom_velocity = (0.0, 0.0);
            return Rect::full_frame();
        }
        center_x = center_x.clamp(width * 0.5, 1.0 - width * 0.5);
        center_y = center_y.clamp(height * 0.5, 1.0 - height * 0.5);
        if (center_x - current_center.0).abs() > max_pan_delta + 0.000_001
            || (center_y - current_center.1).abs() > max_pan_delta + 0.000_001
        {
            self.pan_velocity = (0.0, 0.0);
            self.zoom_velocity = (0.0, 0.0);
            return Rect::full_frame();
        }
        let needed_width = 2.0 * (target_center_x - center_x).abs() + target.width;
        let needed_height = 2.0 * (target_center_y - center_y).abs() + target.height;
        if needed_width > width + 0.000_001 || needed_height > height + 0.000_001 {
            self.pan_velocity = (0.0, 0.0);
            self.zoom_velocity = (0.0, 0.0);
            return Rect::full_frame();
        }
        bounded_at(center_x, center_y, width, height)
    }

    fn crop_aspect(&self) -> f32 {
        self.config.output_aspect / self.config.source_aspect
    }

    fn plan_for(&self, crop: Rect) -> Result<FramePlan, FramingError> {
        let crop_pixel_aspect = (f64::from(self.config.source_aspect) * f64::from(crop.width)
            / f64::from(crop.height)) as f32;
        let output_viewport = if crop == Rect::full_frame() {
            fit_viewport(self.config.source_aspect, self.config.output_aspect)
        } else {
            if !crop_pixel_aspect.is_finite() || crop_pixel_aspect <= 0.0 {
                return Err(FramingError::InvalidTarget);
            }
            fit_viewport(crop_pixel_aspect, self.config.output_aspect)
        };
        Rect::normalized(
            output_viewport.x,
            output_viewport.y,
            output_viewport.width,
            output_viewport.height,
        )
        .map_err(|_| FramingError::InvalidTarget)?;
        Ok(FramePlan {
            source_crop: crop,
            output_viewport,
        })
    }
}

fn bounded_delta(value: f32, limit: f32) -> f32 {
    value.clamp(-limit, limit)
}

#[allow(clippy::too_many_arguments)]
fn advance_axis(
    current: f32,
    target: f32,
    previous_velocity: f32,
    max_step: f32,
    max_velocity: f32,
    max_acceleration: f32,
    smoothing: f32,
    dt: f32,
) -> (f32, f32) {
    let requested_delta = bounded_delta(target - current, max_step) * smoothing;
    let requested_velocity = (requested_delta / dt).clamp(-max_velocity, max_velocity);
    let velocity_delta = bounded_delta(
        requested_velocity - previous_velocity,
        max_acceleration * dt,
    );
    let velocity = (previous_velocity + velocity_delta).clamp(-max_velocity, max_velocity);
    let delta = bounded_delta(velocity * dt, max_step);
    (current + delta, delta / dt)
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
        let speaker = target(0.49, 0.49, 0.01, 0.01);
        let mut plan = controller.plan(speaker).unwrap();
        for _ in 0..100 {
            plan = controller.plan(speaker).unwrap();
        }
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
        let speaker = target(0.4, 0.3, 0.08, 0.16);
        let mut plan = controller.plan(speaker).unwrap();
        for _ in 0..100 {
            plan = controller.plan(speaker).unwrap();
        }
        assert!(
            (plan.source_crop.width / plan.source_crop.height - 81.0 / 256.0).abs() < 0.000_1,
            "crop {:?}; desired {:?}",
            plan.source_crop,
            controller.desired(speaker)
        );
    }

    #[test]
    fn smoothing_preserves_aspect_and_target_containment() {
        let mut controller = FramingController::new(FramingConfig {
            pan_smoothing: 0.5,
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
            pan_smoothing: 0.5,
            max_pan_step: 0.2,
            ..FramingConfig::default()
        })
        .unwrap();
        let first_target = target(0.46, 0.46, 0.08, 0.08);
        let mut first = controller.plan(first_target).unwrap();
        for _ in 0..100 {
            first = controller.plan(first_target).unwrap();
        }
        let held = controller.plan(target(0.49, 0.46, 0.08, 0.08)).unwrap();
        let first_center_x = first.source_crop.x + first.source_crop.width * 0.5;
        let held_center_x = held.source_crop.x + held.source_crop.width * 0.5;
        let first_center_y = first.source_crop.y + first.source_crop.height * 0.5;
        let held_center_y = held.source_crop.y + held.source_crop.height * 0.5;
        assert!((held_center_x - first_center_x).abs() < 0.000_1);
        assert!((held_center_y - first_center_y).abs() < 0.000_1);

        let moving_target = target(0.57, 0.46, 0.08, 0.08);
        let mut moving = controller.plan(moving_target).unwrap();
        if moving.source_crop == Rect::full_frame() {
            moving = controller.plan(moving_target).unwrap();
        }
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
            pan_smoothing: 0.5,
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
    fn horizontal_dead_zone_exit_does_not_pan_vertically() {
        let mut controller = FramingController::new(FramingConfig {
            dead_zone_fraction: 0.1,
            pan_smoothing: 0.5,
            max_pan_step: 0.2,
            ..FramingConfig::default()
        })
        .unwrap();
        let centered_target = target(0.46, 0.46, 0.08, 0.08);
        let mut first = controller.plan(centered_target).unwrap();
        for _ in 0..100 {
            first = controller.plan(centered_target).unwrap();
        }
        let moved_target = target(0.7, 0.46, 0.08, 0.08);
        let mut moved = controller.plan(moved_target).unwrap();
        if moved.source_crop == Rect::full_frame() {
            moved = controller.plan(moved_target).unwrap();
        }
        let first_center_x = first.source_crop.x + first.source_crop.width * 0.5;
        let first_center_y = first.source_crop.y + first.source_crop.height * 0.5;
        let moved_center_x = moved.source_crop.x + moved.source_crop.width * 0.5;
        let moved_center_y = moved.source_crop.y + moved.source_crop.height * 0.5;

        assert!(moved_center_x > first_center_x);
        assert!((moved_center_y - first_center_y).abs() < 0.000_1);
        assert!(contains(moved.source_crop, moved_target));
    }

    #[test]
    fn pan_and_zoom_smoothing_are_independently_configurable() {
        let make_controller = |pan_smoothing, zoom_smoothing| {
            FramingController::new(FramingConfig {
                dead_zone_fraction: 0.0,
                max_zoom: 8.0,
                max_pan_step: 0.2,
                max_zoom_step: 0.2,
                pan_smoothing,
                zoom_smoothing,
                ..FramingConfig::default()
            })
            .unwrap()
        };

        let mut fast_pan = make_controller(0.8, 0.1);
        let mut slow_pan = make_controller(0.1, 0.1);
        let initial = target(0.46, 0.46, 0.08, 0.08);
        fast_pan.plan(initial).unwrap();
        slow_pan.plan(initial).unwrap();
        let moved_target = target(0.53, 0.46, 0.08, 0.08);
        let fast_pan_plan = fast_pan.plan(moved_target).unwrap();
        let slow_pan_plan = slow_pan.plan(moved_target).unwrap();
        let fast_pan_center = fast_pan_plan.source_crop.x + fast_pan_plan.source_crop.width * 0.5;
        let slow_pan_center = slow_pan_plan.source_crop.x + slow_pan_plan.source_crop.width * 0.5;
        assert!(fast_pan_center > slow_pan_center);

        let mut fast_zoom = make_controller(0.8, 0.8);
        let mut slow_zoom = make_controller(0.8, 0.1);
        let initial = target(0.46, 0.46, 0.08, 0.08);
        fast_zoom.plan(initial).unwrap();
        slow_zoom.plan(initial).unwrap();
        let smaller_target = target(0.48, 0.48, 0.04, 0.04);
        let fast_zoom_plan = fast_zoom.plan(smaller_target).unwrap();
        let slow_zoom_plan = slow_zoom.plan(smaller_target).unwrap();
        assert!(fast_zoom_plan.source_crop.width < slow_zoom_plan.source_crop.width);
    }

    #[test]
    fn large_pan_that_breaks_step_limit_falls_back_to_safe_wide() {
        let mut controller = FramingController::new(FramingConfig::default()).unwrap();
        let first_target = target(0.2, 0.3, 0.08, 0.16);
        for _ in 0..100 {
            controller.plan(first_target).unwrap();
        }
        let plan = controller.plan(target(0.9, 0.3, 0.08, 0.16)).unwrap();
        assert_eq!(plan.source_crop, Rect::full_frame());
        assert_eq!(plan.output_viewport, Rect::full_frame());
    }

    #[test]
    fn first_selected_target_moves_and_zooms_from_safe_wide_with_bounds() {
        let mut controller = FramingController::new(FramingConfig {
            pan_smoothing: 1.0,
            max_pan_step: 0.1,
            zoom_smoothing: 1.0,
            max_zoom_step: 0.2,
            ..FramingConfig::default()
        })
        .unwrap();
        let speaker = target(0.2, 0.2, 0.1, 0.2);
        let plan = controller.plan(speaker).unwrap();
        let crop = plan.source_crop;
        let center_x = crop.x + crop.width * 0.5;
        let center_y = crop.y + crop.height * 0.5;

        assert!(contains(crop, speaker));
        assert!((center_x - 0.5).abs() <= 0.1);
        assert!((center_y - 0.5).abs() <= 0.1);
        assert!((1.0 - crop.width) <= 0.2);
        assert!((1.0 - crop.height) <= 0.2);
        assert_ne!(crop, controller.desired(speaker));
    }

    #[test]
    fn mismatched_aspect_intermediate_crops_use_fit_padding() {
        let source_aspect = 4.0 / 3.0;
        let output_aspect = 16.0 / 9.0;
        let mut controller = FramingController::new(FramingConfig {
            source_aspect,
            output_aspect,
            ..FramingConfig::default()
        })
        .unwrap();
        let speaker = target(0.4, 0.3, 0.08, 0.16);
        let plan = controller.plan(speaker).unwrap();
        let crop_pixel_aspect = source_aspect * plan.source_crop.width / plan.source_crop.height;
        let viewport_aspect = plan.output_viewport.width / plan.output_viewport.height;

        assert!(contains(plan.source_crop, speaker));
        assert!(plan.output_viewport.width < 1.0);
        assert!((viewport_aspect - crop_pixel_aspect / output_aspect).abs() < 0.000_1);
    }

    #[test]
    fn crop_recovers_smoothly_after_wide_fallback() {
        let mut controller = FramingController::new(FramingConfig::default()).unwrap();
        let first_target = target(0.2, 0.3, 0.08, 0.16);
        for _ in 0..100 {
            controller.plan(first_target).unwrap();
        }
        let distant_target = target(0.9, 0.3, 0.08, 0.16);
        let wide = controller.plan(distant_target).unwrap();
        assert_eq!(wide.source_crop, Rect::full_frame());

        let recovered = controller.plan(distant_target).unwrap();
        let crop = recovered.source_crop;
        let center_x = crop.x + crop.width * 0.5;
        let center_y = crop.y + crop.height * 0.5;
        assert!(contains(crop, distant_target));
        assert_ne!(crop, controller.desired(distant_target));
        assert!((center_x - 0.5).abs() <= 0.08 * 0.25);
        assert!((center_y - 0.5).abs() <= 0.08 * 0.25);
    }

    #[test]
    fn event_timed_pan_respects_acceleration_bound() {
        let mut controller = FramingController::new(FramingConfig {
            pan_smoothing: 1.0,
            dead_zone_fraction: 0.0,
            ..FramingConfig::default()
        })
        .unwrap();
        let initial = controller.plan_at(target(0.2, 0.4, 0.1, 0.1), 0).unwrap();
        let reversal = controller.plan_at(target(0.7, 0.4, 0.1, 0.1), 33).unwrap();
        let following = controller.plan_at(target(0.7, 0.4, 0.1, 0.1), 66).unwrap();

        let center = |crop: Rect| crop.x + crop.width * 0.5;
        let dt = 0.033_f32;
        let first_velocity = (center(reversal.source_crop) - center(initial.source_crop)) / dt;
        let second_velocity = (center(following.source_crop) - center(reversal.source_crop)) / dt;
        let maximum_acceleration = (FramingConfig::default().max_pan_step
            / NOMINAL_SAMPLE_PERIOD_SECONDS)
            / FULL_SPEED_RAMP_SECONDS;

        assert!(first_velocity.abs() <= maximum_acceleration * dt + 0.001);
        assert!(
            (second_velocity - first_velocity).abs() <= maximum_acceleration * dt + 0.001,
            "velocity changed from {first_velocity} to {second_velocity}"
        );
    }

    #[test]
    fn event_timed_zoom_respects_acceleration_bound() {
        let mut controller = FramingController::new(FramingConfig {
            zoom_smoothing: 1.0,
            dead_zone_fraction: 0.0,
            ..FramingConfig::default()
        })
        .unwrap();
        let initial = controller.plan_at(target(0.4, 0.4, 0.2, 0.2), 0).unwrap();
        let reversal = controller
            .plan_at(target(0.45, 0.45, 0.1, 0.1), 33)
            .unwrap();
        let following = controller
            .plan_at(target(0.45, 0.45, 0.1, 0.1), 66)
            .unwrap();

        let dt = 0.033_f32;
        let first_velocity = (reversal.source_crop.width - initial.source_crop.width) / dt;
        let second_velocity = (following.source_crop.width - reversal.source_crop.width) / dt;
        let maximum_acceleration = (FramingConfig::default().max_zoom_step
            / NOMINAL_SAMPLE_PERIOD_SECONDS)
            / FULL_SPEED_RAMP_SECONDS;

        assert!(
            (second_velocity - first_velocity).abs() <= maximum_acceleration * dt + 0.001,
            "zoom velocity changed from {first_velocity} to {second_velocity}"
        );
    }

    #[test]
    fn non_monotonic_plan_time_is_a_strict_no_op() {
        let mut controller = FramingController::new(FramingConfig::default()).unwrap();
        controller.plan_at(target(0.4, 0.4, 0.1, 0.1), 100).unwrap();
        let before = (
            controller.current,
            controller.last_plan_at_ms,
            controller.pan_velocity,
            controller.zoom_velocity,
        );

        assert_eq!(
            controller.plan_at(target(0.6, 0.4, 0.1, 0.1), 99),
            Err(FramingError::NonMonotonicTime)
        );
        assert_eq!(
            (
                controller.current,
                controller.last_plan_at_ms,
                controller.pan_velocity,
                controller.zoom_velocity,
            ),
            before
        );
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
        assert!(matches!(
            FramingController::new(FramingConfig {
                pan_smoothing: 0.0,
                ..FramingConfig::default()
            }),
            Err(FramingError::InvalidConfig)
        ));
        assert!(matches!(
            FramingController::new(FramingConfig {
                zoom_smoothing: f32::INFINITY,
                ..FramingConfig::default()
            }),
            Err(FramingError::InvalidConfig)
        ));
        assert!(matches!(
            FramingController::new(FramingConfig {
                max_pan_step: 1.01,
                ..FramingConfig::default()
            }),
            Err(FramingError::InvalidConfig)
        ));
        assert!(matches!(
            FramingController::new(FramingConfig {
                max_zoom_step: 1.01,
                ..FramingConfig::default()
            }),
            Err(FramingError::InvalidConfig)
        ));
    }
}
