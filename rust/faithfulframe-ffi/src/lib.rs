//! Narrow C ABI. Unsafe pointer handling is contained in this crate only.

use faithfulframe_core::{
    Config, CropAction, Generations, Reason, Rect, Snapshot, State, TrackingSession,
};

#[repr(C)]
#[derive(Clone, Copy)]
pub struct FfGenerations {
    source: u64,
    session: u64,
    stream: u64,
    geometry: u64,
    tracker: u64,
}
#[repr(C)]
#[derive(Clone, Copy)]
pub struct FfRect {
    x: f32,
    y: f32,
    width: f32,
    height: f32,
}
#[repr(C)]
#[derive(Clone, Copy)]
pub struct FfSessionConfig {
    loss_grace_ms: u64,
    max_evidence_age_ms: u64,
    max_candidates: u32,
    safe_wide: FfRect,
}
#[repr(C)]
pub struct FfSnapshot {
    state: u32,
    reason: u32,
    crop_action: u32,
    status_version: u64,
    observation_version: u64,
    loss_deadline_ms: u64,
    has_selected: u8,
    has_loss_deadline: u8,
    crop: FfRect,
}
pub struct FfSession {
    inner: TrackingSession,
}

const OK: i32 = 0;
const INVALID_ARGUMENT: i32 = 1;
const PANIC: i32 = 3;

fn generations(value: FfGenerations) -> Generations {
    Generations {
        source: value.source,
        session: value.session,
        stream: value.stream,
        geometry: value.geometry,
        tracker: value.tracker,
    }
}
fn rect(value: FfRect) -> Result<Rect, i32> {
    Rect::normalized(value.x, value.y, value.width, value.height).map_err(|_| INVALID_ARGUMENT)
}
fn map_state(value: State) -> u32 {
    match value {
        State::Frozen => 0,
        State::Manual => 1,
        State::Searching => 2,
        State::Locked => 3,
        State::Lost => 4,
    }
}
fn map_reason(value: Reason) -> u32 {
    value as u32
}
fn map_crop(value: CropAction) -> u32 {
    value as u32
}
fn write_snapshot(snapshot: Snapshot, output: &mut FfSnapshot) {
    *output = FfSnapshot {
        state: map_state(snapshot.state),
        reason: map_reason(snapshot.reason),
        crop_action: map_crop(snapshot.crop_action),
        status_version: snapshot.status_version,
        observation_version: snapshot.observation_version,
        loss_deadline_ms: snapshot.loss_deadline.unwrap_or(0),
        has_selected: snapshot.selected.is_some() as u8,
        has_loss_deadline: snapshot.loss_deadline.is_some() as u8,
        crop: FfRect {
            x: snapshot.crop.x,
            y: snapshot.crop.y,
            width: snapshot.crop.width,
            height: snapshot.crop.height,
        },
    };
}

#[unsafe(no_mangle)]
pub extern "C" fn faithfulframe_ffi_abi_version() -> u32 {
    1
}

#[unsafe(no_mangle)]
/// # Safety
/// `config`, `generations_in`, and `output` must be valid for the call.
pub unsafe extern "C" fn ff_session_create(
    config: *const FfSessionConfig,
    generations_in: *const FfGenerations,
    output: *mut *mut FfSession,
) -> i32 {
    std::panic::catch_unwind(|| unsafe {
        if config.is_null() || generations_in.is_null() || output.is_null() {
            return INVALID_ARGUMENT;
        }
        let config = &*config;
        let safe_wide = match rect(config.safe_wide) {
            Ok(value) => value,
            Err(error) => return error,
        };
        match TrackingSession::new(
            Config {
                loss_grace_ms: config.loss_grace_ms,
                max_evidence_age_ms: config.max_evidence_age_ms,
                max_candidates: config.max_candidates as usize,
                safe_wide,
            },
            generations(*generations_in),
        ) {
            Ok(session) => {
                *output = Box::into_raw(Box::new(FfSession { inner: session }));
                OK
            }
            Err(_) => INVALID_ARGUMENT,
        }
    })
    .unwrap_or(PANIC)
}

#[unsafe(no_mangle)]
/// # Safety
/// `session` must be null or a pointer returned once by `ff_session_create`.
pub unsafe extern "C" fn ff_session_destroy(session: *mut FfSession) {
    if !session.is_null() {
        unsafe {
            drop(Box::from_raw(session));
        }
    }
}
#[unsafe(no_mangle)]
/// # Safety
/// `session` and `output` must be valid for the call and must not alias mutably.
pub unsafe extern "C" fn ff_session_snapshot(
    session: *const FfSession,
    output: *mut FfSnapshot,
) -> i32 {
    unsafe {
        if session.is_null() || output.is_null() {
            return INVALID_ARGUMENT;
        }
        write_snapshot((*session).inner.snapshot(), &mut *output);
        OK
    }
}
#[unsafe(no_mangle)]
/// # Safety
/// `session` and `output` must be valid for the call and must not alias mutably.
pub unsafe extern "C" fn ff_session_tick(
    session: *mut FfSession,
    at: u64,
    output: *mut FfSnapshot,
) -> i32 {
    std::panic::catch_unwind(|| unsafe {
        if session.is_null() || output.is_null() {
            return INVALID_ARGUMENT;
        }
        write_snapshot(
            (*session)
                .inner
                .apply(faithfulframe_core::Event::Tick { at }),
            &mut *output,
        );
        OK
    })
    .unwrap_or(PANIC)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn abi_version_is_stable() {
        assert_eq!(faithfulframe_ffi_abi_version(), 1);
    }
}
