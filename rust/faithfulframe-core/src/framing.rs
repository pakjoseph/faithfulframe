//! Pure normalized crop planning with bounded pan and zoom smoothing.

use crate::Rect;

#[derive(Clone, Copy, Debug)]
pub struct FramingConfig {
    pub output_aspect: f32,
    pub target_coverage: f32,
    pub min_zoom: f32,
    pub max_zoom: f32,
    pub smoothing: f32,
    pub max_pan_step: f32,
    pub max_zoom_step: f32,
}

impl Default for FramingConfig {
    fn default() -> Self {
        Self {
            output_aspect: 16.0 / 9.0,
            target_coverage: 0.45,
            min_zoom: 1.0,
            max_zoom: 4.0,
            smoothing: 0.25,
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

/// The controller owns no clocks, threads, or external resources. The caller
/// invokes `plan` once per authorized target update on the session executor.
pub struct FramingController {
    config: FramingConfig,
    current: Option<Rect>,
}

impl FramingController {
    pub fn new(config: FramingConfig) -> Result<Self, FramingError> {
        if !config.output_aspect.is_finite()
            || !config.target_coverage.is_finite()
            || !config.min_zoom.is_finite()
            || !config.max_zoom.is_finite()
            || !config.smoothing.is_finite()
            || !config.max_pan_step.is_finite()
            || !config.max_zoom_step.is_finite()
            || config.output_aspect <= 0.0
            || !(0.0 < config.target_coverage && config.target_coverage <= 1.0)
            || config.min_zoom < 1.0
            || config.max_zoom < config.min_zoom
            || !(0.0 < config.smoothing && config.smoothing <= 1.0)
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

    /// Returns an aspect-correct source crop containing target, or full source
    /// when the requested target cannot safely fit under the configured limits.
    pub fn plan(&mut self, target: Rect) -> Result<Rect, FramingError> {
        Rect::normalized(target.x, target.y, target.width, target.height)
            .map_err(|_| FramingError::InvalidTarget)?;
        let desired = self.desired(target);
        let next = self
            .current
            .map_or(desired, |current| self.smooth(current, desired, target));
        self.current = Some(next);
        Ok(next)
    }

    fn desired(&self, target: Rect) -> Rect {
        let desired_width = (target.width / self.config.target_coverage)
            .max(target.height * self.config.output_aspect / self.config.target_coverage);
        let width = desired_width.max(1.0 / self.config.max_zoom);
        if width > 1.0 / self.config.min_zoom {
            return Rect::full_frame();
        }
        let height = width / self.config.output_aspect;
        if height > 1.0 || height < target.height {
            return Rect::full_frame();
        }
        bounded_centered(target, width, height)
    }

    fn smooth(&self, current: Rect, desired: Rect, target: Rect) -> Rect {
        if desired == Rect::full_frame() {
            return desired;
        }
        let current_center = (
            current.x + current.width * 0.5,
            current.y + current.height * 0.5,
        );
        let desired_center = (
            desired.x + desired.width * 0.5,
            desired.y + desired.height * 0.5,
        );
        let center_x = current_center.0
            + bounded_delta(
                desired_center.0 - current_center.0,
                self.config.max_pan_step,
            ) * self.config.smoothing;
        let center_y = current_center.1
            + bounded_delta(
                desired_center.1 - current_center.1,
                self.config.max_pan_step,
            ) * self.config.smoothing;
        let width = current.width
            + bounded_delta(desired.width - current.width, self.config.max_zoom_step)
                * self.config.smoothing;
        let height = current.height
            + bounded_delta(desired.height - current.height, self.config.max_zoom_step)
                * self.config.smoothing;
        let smoothed = bounded_at(center_x, center_y, width, height);
        if contains(smoothed, target) {
            smoothed
        } else {
            desired
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

fn contains(outer: Rect, inner: Rect) -> bool {
    outer.x <= inner.x
        && outer.y <= inner.y
        && outer.x + outer.width >= inner.x + inner.width
        && outer.y + outer.height >= inner.y + inner.height
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
        let crop = controller.plan(speaker).unwrap();
        assert!(contains(crop, speaker));
        assert!((crop.width / crop.height - 16.0 / 9.0).abs() < 0.000_1);
        assert!(crop.x >= 0.0 && crop.y >= 0.0);
        assert!(crop.x + crop.width <= 1.0 && crop.y + crop.height <= 1.0);
    }

    #[test]
    fn edge_target_is_kept_in_bounds() {
        let mut controller = FramingController::new(FramingConfig::default()).unwrap();
        let crop = controller.plan(target(0.92, 0.7, 0.08, 0.2)).unwrap();
        assert!(crop.x + crop.width <= 1.0);
        assert!(crop.y + crop.height <= 1.0);
    }

    #[test]
    fn oversized_target_falls_back_to_full_source() {
        let mut controller = FramingController::new(FramingConfig::default()).unwrap();
        assert_eq!(
            controller.plan(target(0.0, 0.0, 1.0, 1.0)).unwrap(),
            Rect::full_frame()
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
    }
}
