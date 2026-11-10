//! Narrow C ABI. Unsafe pointer handling is contained in this crate only.

use faithfulframe_core::{
    Candidate, Command, CommandEnvelope, CommandResult, Config, CropAction, Event, Generations,
    Identity, Lifecycle, Reason, Rect, Snapshot, State, TrackingSession,
};
use std::panic::{AssertUnwindSafe, catch_unwind};

const OK: i32 = 0;
const INVALID_ARGUMENT: i32 = 1;
const PANIC: i32 = 2;

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
pub struct FfIdentity {
    source: u64,
    session: u64,
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
#[derive(Clone, Copy)]
pub struct FfCandidate {
    handle: u64,
    continuity_token: u64,
    identity: FfIdentity,
    observation_version: u64,
    generations: FfGenerations,
    observed_at_ms: u64,
    expires_at_ms: u64,
    rect: FfRect,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct FfCommand {
    kind: u32,
    identity: FfIdentity,
    status_version: u64,
    generations: FfGenerations,
    candidate_handle: u64,
    observation_version: u64,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct FfLifecycle {
    kind: u32,
    generations: FfGenerations,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct FfSnapshot {
    state: u32,
    reason: u32,
    crop_action: u32,
    identity: FfIdentity,
    status_version: u64,
    observation_version: u64,
    selected_handle: u64,
    loss_deadline_ms: u64,
    has_selected: u8,
    has_loss_deadline: u8,
    command_result: u32,
    rejection_reason: u32,
    crop: FfRect,
    generations: FfGenerations,
}

pub struct FfSession {
    inner: TrackingSession,
}

fn generations(value: FfGenerations) -> Generations {
    Generations {
        source: value.source,
        session: value.session,
        stream: value.stream,
        geometry: value.geometry,
        tracker: value.tracker,
    }
}
fn identity(value: FfIdentity) -> Identity {
    Identity {
        source: value.source,
        session: value.session,
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
    match value {
        Reason::Startup => 0,
        Reason::OperatorFreeze => 1,
        Reason::ManualOverride => 2,
        Reason::AwaitingSelection => 3,
        Reason::SelectedByOperator => 4,
        Reason::ContinuityRecovered => 5,
        Reason::TargetEvidenceStale => 6,
        Reason::TargetLossDeadlineExpired => 7,
        Reason::ResetWide => 8,
        Reason::LifecycleInvalidated => 9,
        Reason::StaleCommand => 10,
        Reason::InvalidCommand => 11,
        Reason::NonMonotonicEvent => 12,
        Reason::InvalidCandidate => 13,
    }
}
fn map_crop(value: CropAction) -> u32 {
    match value {
        CropAction::Hold => 0,
        CropAction::Wide => 1,
        CropAction::Track => 2,
        CropAction::None => 3,
    }
}
fn write_snapshot(snapshot: Snapshot, output: &mut FfSnapshot) {
    let (command_result, rejection_reason) = match snapshot.command_result {
        CommandResult::NotACommand => (0, 0),
        CommandResult::Applied => (1, 0),
        CommandResult::Rejected(reason) => {
            let code = match reason {
                Reason::StaleCommand => 1,
                Reason::InvalidCommand => 2,
                Reason::NonMonotonicEvent => 3,
                Reason::InvalidCandidate => 4,
                _ => 2,
            };
            (2, code)
        }
    };
    *output = FfSnapshot {
        state: map_state(snapshot.state),
        reason: map_reason(snapshot.reason),
        crop_action: map_crop(snapshot.crop_action),
        identity: FfIdentity {
            source: snapshot.identity.source,
            session: snapshot.identity.session,
        },
        status_version: snapshot.status_version,
        observation_version: snapshot.observation_version,
        selected_handle: snapshot.selected.unwrap_or(0),
        loss_deadline_ms: snapshot.loss_deadline.unwrap_or(0),
        has_selected: snapshot.selected.is_some() as u8,
        has_loss_deadline: snapshot.loss_deadline.is_some() as u8,
        command_result,
        rejection_reason,
        crop: FfRect {
            x: snapshot.crop.x,
            y: snapshot.crop.y,
            width: snapshot.crop.width,
            height: snapshot.crop.height,
        },
        generations: FfGenerations {
            source: snapshot.generations.source,
            session: snapshot.generations.session,
            stream: snapshot.generations.stream,
            geometry: snapshot.generations.geometry,
            tracker: snapshot.generations.tracker,
        },
    };
}
fn command(value: FfCommand) -> Result<Command, i32> {
    Ok(match value.kind {
        0 => Command::EnableAutoFollow,
        1 => Command::Freeze,
        2 => Command::Resume,
        3 => Command::ManualOverride,
        4 => Command::SelectCandidate {
            handle: value.candidate_handle,
            observation_version: value.observation_version,
        },
        5 => Command::ResetToWide,
        _ => return Err(INVALID_ARGUMENT),
    })
}
fn lifecycle(value: FfLifecycle) -> Result<Lifecycle, i32> {
    Ok(match value.kind {
        0 => Lifecycle::SceneInactive,
        1 => Lifecycle::SceneActive,
        2 => Lifecycle::CameraDisconnected,
        3 => Lifecycle::CameraRestored,
        4 => Lifecycle::SourceRebound {
            generations: generations(value.generations),
        },
        5 => Lifecycle::GeometryChanged {
            generations: generations(value.generations),
        },
        6 => Lifecycle::TrackerChanged {
            generations: generations(value.generations),
        },
        _ => return Err(INVALID_ARGUMENT),
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn faithfulframe_ffi_abi_version() -> u32 {
    3
}

#[unsafe(no_mangle)]
/// # Safety
/// All pointers must reference valid records for the duration of this call.
pub unsafe extern "C" fn ff_session_create(
    config: *const FfSessionConfig,
    identity_in: *const FfIdentity,
    generations_in: *const FfGenerations,
    output: *mut *mut FfSession,
) -> i32 {
    catch_unwind(AssertUnwindSafe(|| unsafe {
        if config.is_null() || identity_in.is_null() || generations_in.is_null() || output.is_null()
        {
            return INVALID_ARGUMENT;
        }
        *output = std::ptr::null_mut();
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
            identity(*identity_in),
            generations(*generations_in),
        ) {
            Ok(session) => {
                *output = Box::into_raw(Box::new(FfSession { inner: session }));
                OK
            }
            Err(_) => INVALID_ARGUMENT,
        }
    }))
    .unwrap_or(PANIC)
}

#[unsafe(no_mangle)]
/// # Safety
/// Session must be null or a live pointer returned once by ff_session_create.
pub unsafe extern "C" fn ff_session_destroy(session: *mut FfSession) {
    if !session.is_null() {
        let _ = catch_unwind(AssertUnwindSafe(|| unsafe {
            drop(Box::from_raw(session));
        }));
    }
}

#[unsafe(no_mangle)]
/// # Safety
/// Pointers must be valid. All access to one session must be serialized by its owner.
pub unsafe extern "C" fn ff_session_snapshot(
    session: *const FfSession,
    output: *mut FfSnapshot,
) -> i32 {
    catch_unwind(AssertUnwindSafe(|| unsafe {
        if session.is_null() || output.is_null() {
            return INVALID_ARGUMENT;
        }
        write_snapshot((*session).inner.snapshot(), &mut *output);
        OK
    }))
    .unwrap_or(PANIC)
}

#[unsafe(no_mangle)]
/// # Safety
/// Pointers must be valid. All access to one session must be serialized by its owner.
pub unsafe extern "C" fn ff_session_tick(
    session: *mut FfSession,
    at: u64,
    output: *mut FfSnapshot,
) -> i32 {
    catch_unwind(AssertUnwindSafe(|| unsafe {
        if session.is_null() || output.is_null() {
            return INVALID_ARGUMENT;
        }
        write_snapshot((*session).inner.apply(Event::Tick { at }), &mut *output);
        OK
    }))
    .unwrap_or(PANIC)
}

#[unsafe(no_mangle)]
/// # Safety
/// Pointers must be valid. All access to one session must be serialized by its owner.
pub unsafe extern "C" fn ff_session_publish_candidate(
    session: *mut FfSession,
    at: u64,
    input: *const FfCandidate,
    output: *mut FfSnapshot,
) -> i32 {
    catch_unwind(AssertUnwindSafe(|| unsafe {
        if session.is_null() || input.is_null() || output.is_null() {
            return INVALID_ARGUMENT;
        }
        let input = &*input;
        let candidate_rect = match rect(input.rect) {
            Ok(value) => value,
            Err(error) => return error,
        };
        let candidate = Candidate {
            handle: input.handle,
            continuity: input.continuity_token,
            identity: identity(input.identity),
            observation_version: input.observation_version,
            generations: generations(input.generations),
            observed_at: input.observed_at_ms,
            expires_at: input.expires_at_ms,
            rect: candidate_rect,
        };
        write_snapshot(
            (*session).inner.apply(Event::Candidate { at, candidate }),
            &mut *output,
        );
        OK
    }))
    .unwrap_or(PANIC)
}

#[unsafe(no_mangle)]
/// # Safety
/// Pointers must be valid. All access to one session must be serialized by its owner.
pub unsafe extern "C" fn ff_session_apply_command(
    session: *mut FfSession,
    at: u64,
    input: *const FfCommand,
    output: *mut FfSnapshot,
) -> i32 {
    catch_unwind(AssertUnwindSafe(|| unsafe {
        if session.is_null() || input.is_null() || output.is_null() {
            return INVALID_ARGUMENT;
        }
        let input = *input;
        let command = match command(input) {
            Ok(value) => value,
            Err(error) => return error,
        };
        let event = Event::Command {
            at,
            envelope: CommandEnvelope {
                identity: identity(input.identity),
                status_version: input.status_version,
                generations: generations(input.generations),
                command,
            },
        };
        write_snapshot((*session).inner.apply(event), &mut *output);
        OK
    }))
    .unwrap_or(PANIC)
}

#[unsafe(no_mangle)]
/// # Safety
/// Pointers must be valid. All access to one session must be serialized by its owner.
pub unsafe extern "C" fn ff_session_lifecycle(
    session: *mut FfSession,
    at: u64,
    input: *const FfLifecycle,
    output: *mut FfSnapshot,
) -> i32 {
    catch_unwind(AssertUnwindSafe(|| unsafe {
        if session.is_null() || input.is_null() || output.is_null() {
            return INVALID_ARGUMENT;
        }
        let lifecycle = match lifecycle(*input) {
            Ok(value) => value,
            Err(error) => return error,
        };
        write_snapshot(
            (*session).inner.apply(Event::Lifecycle {
                at,
                event: lifecycle,
            }),
            &mut *output,
        );
        OK
    }))
    .unwrap_or(PANIC)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn abi_version_is_stable() {
        assert_eq!(faithfulframe_ffi_abi_version(), 3);
    }
}
