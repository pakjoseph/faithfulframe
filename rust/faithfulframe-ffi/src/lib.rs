//! Narrow C ABI. Unsafe pointer handling is contained in this crate only.

use faithfulframe_core::framing::FramingConfig;
use faithfulframe_core::{
    Candidate, Command, CommandEnvelope, CommandResult, Config, CropAction, Event, Generations,
    Identity, Lifecycle, Reason, Rect, Snapshot, State, TrackingEngine,
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
    source_width: u32,
    source_height: u32,
    output_width: u32,
    output_height: u32,
    target_coverage: f32,
    min_zoom: f32,
    max_zoom: f32,
    pan_smoothing: f32,
    max_pan_step: f32,
    max_zoom_step: f32,
    dead_zone_fraction: f32,
    zoom_smoothing: f32,
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
    identity: FfIdentity,
    generations: FfGenerations,
    source_width: u32,
    source_height: u32,
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
    closed: u8,
    command_result: u32,
    rejection_reason: u32,
    crop: FfRect,
    viewport: FfRect,
    generations: FfGenerations,
}

// The public C header intentionally mirrors these repr(C) value records.
// Fail the Rust build if a field addition or platform layout change drifts
// from the C++ bridge's compile-time ABI assertions.
const _: () = {
    assert!(std::mem::size_of::<FfGenerations>() == 40);
    assert!(std::mem::size_of::<FfIdentity>() == 16);
    assert!(std::mem::size_of::<FfRect>() == 16);
    assert!(std::mem::size_of::<FfSessionConfig>() == 72);
    assert!(std::mem::size_of::<FfCandidate>() == 112);
    assert!(std::mem::size_of::<FfCommand>() == 88);
    assert!(std::mem::size_of::<FfLifecycle>() == 72);
    assert!(std::mem::size_of::<FfSnapshot>() == 152);
    assert!(std::mem::offset_of!(FfSnapshot, generations) == 112);
};

pub struct FfSession {
    inner: TrackingEngine,
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
fn rect(value: FfRect) -> Rect {
    // Preserve raw input so the serialized policy event can reject malformed
    // geometry after independently advancing any due lifecycle timers.
    Rect {
        x: value.x,
        y: value.y,
        width: value.width,
        height: value.height,
    }
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
        Reason::TrackerIdentityExpired => 14,
        Reason::SessionClosing => 15,
        Reason::SessionClosed => 16,
        Reason::StaleLifecycle => 17,
        Reason::InvalidLifecycle => 18,
        Reason::FramingFailure => 19,
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
                Reason::StaleLifecycle => 5,
                Reason::InvalidLifecycle => 6,
                Reason::SessionClosed => 7,
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
        closed: snapshot.closed as u8,
        command_result,
        rejection_reason,
        crop: FfRect {
            x: snapshot.crop.x,
            y: snapshot.crop.y,
            width: snapshot.crop.width,
            height: snapshot.crop.height,
        },
        viewport: FfRect {
            x: snapshot.viewport.x,
            y: snapshot.viewport.y,
            width: snapshot.viewport.width,
            height: snapshot.viewport.height,
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
    let generations = generations(value.generations);
    Ok(match value.kind {
        0 => Lifecycle::SceneInactive { generations },
        1 => Lifecycle::SceneActive { generations },
        2 => Lifecycle::CameraDisconnected { generations },
        3 => Lifecycle::CameraRestored { generations },
        4 => Lifecycle::SourceRebound { generations },
        5 => Lifecycle::GeometryChanged {
            generations,
            source_width: value.source_width,
            source_height: value.source_height,
        },
        6 => Lifecycle::TrackerChanged { generations },
        7 => Lifecycle::FramingConfigChanged { generations },
        8 => Lifecycle::CandidatePolicyChanged { generations },
        9 => Lifecycle::AdapterDestroyed { generations },
        10 => Lifecycle::SourceDestroyed { generations },
        11 => Lifecycle::ObsShutdown { generations },
        _ => return Err(INVALID_ARGUMENT),
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn faithfulframe_ffi_abi_version() -> u32 {
    7
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
        if output.is_null() {
            return INVALID_ARGUMENT;
        }
        *output = std::ptr::null_mut();
        if config.is_null() || identity_in.is_null() || generations_in.is_null() {
            return INVALID_ARGUMENT;
        }
        let config = &*config;
        if config.source_width == 0
            || config.source_height == 0
            || config.output_width == 0
            || config.output_height == 0
        {
            return INVALID_ARGUMENT;
        }
        let framing = FramingConfig {
            source_aspect: config.source_width as f32 / config.source_height as f32,
            output_aspect: config.output_width as f32 / config.output_height as f32,
            target_coverage: config.target_coverage,
            min_zoom: config.min_zoom,
            max_zoom: config.max_zoom,
            pan_smoothing: config.pan_smoothing,
            zoom_smoothing: config.zoom_smoothing,
            dead_zone_fraction: config.dead_zone_fraction,
            max_pan_step: config.max_pan_step,
            max_zoom_step: config.max_zoom_step,
        };
        match TrackingEngine::new(
            Config {
                loss_grace_ms: config.loss_grace_ms,
                max_evidence_age_ms: config.max_evidence_age_ms,
                max_candidates: config.max_candidates as usize,
                ..Config::default()
            },
            framing,
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
        let candidate_rect = rect(input.rect);
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
        let input = *input;
        let lifecycle = match lifecycle(input) {
            Ok(value) => value,
            Err(error) => return error,
        };
        write_snapshot(
            (*session).inner.apply(Event::Lifecycle {
                at,
                identity: identity(input.identity),
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
        assert_eq!(faithfulframe_ffi_abi_version(), 7);
    }

    #[test]
    fn create_clears_output_before_rejecting_invalid_inputs() {
        let mut output = std::ptr::NonNull::<FfSession>::dangling().as_ptr();
        let result = unsafe {
            ff_session_create(
                std::ptr::null(),
                std::ptr::null(),
                std::ptr::null(),
                &mut output,
            )
        };
        assert_eq!(result, INVALID_ARGUMENT);
        assert!(output.is_null());
    }

    #[test]
    fn invalid_dead_zone_config_cannot_create_a_session() {
        let config = FfSessionConfig {
            loss_grace_ms: 5_000,
            max_evidence_age_ms: 500,
            max_candidates: 8,
            source_width: 3_840,
            source_height: 2_160,
            output_width: 1_920,
            output_height: 1_080,
            target_coverage: 0.45,
            min_zoom: 1.0,
            max_zoom: 4.0,
            pan_smoothing: 0.25,
            max_pan_step: 0.08,
            max_zoom_step: 0.08,
            dead_zone_fraction: 0.5,
            zoom_smoothing: 0.25,
        };
        let identity = FfIdentity {
            source: 1,
            session: 1,
        };
        let generations = FfGenerations {
            source: 1,
            session: 1,
            stream: 1,
            geometry: 1,
            tracker: 1,
        };
        let mut output = std::ptr::NonNull::<FfSession>::dangling().as_ptr();

        let result = unsafe { ff_session_create(&config, &identity, &generations, &mut output) };

        assert_eq!(result, INVALID_ARGUMENT);
        assert!(output.is_null());
    }

    #[test]
    fn malformed_candidate_geometry_reaches_policy_and_keeps_due_deadlines() {
        let config = FfSessionConfig {
            loss_grace_ms: 100,
            max_evidence_age_ms: 10,
            max_candidates: 8,
            source_width: 3_840,
            source_height: 2_160,
            output_width: 1_920,
            output_height: 1_080,
            target_coverage: 0.45,
            min_zoom: 1.0,
            max_zoom: 4.0,
            pan_smoothing: 0.25,
            max_pan_step: 0.08,
            max_zoom_step: 0.08,
            dead_zone_fraction: 0.05,
            zoom_smoothing: 0.25,
        };
        let identity = FfIdentity {
            source: 101,
            session: 201,
        };
        let generations = FfGenerations {
            source: 1,
            session: 1,
            stream: 1,
            geometry: 1,
            tracker: 1,
        };
        let mut raw_session = std::ptr::null_mut();
        assert_eq!(
            unsafe { ff_session_create(&config, &identity, &generations, &mut raw_session) },
            OK
        );
        let session = raw_session;
        let mut output: FfSnapshot = unsafe { std::mem::zeroed() };
        let enable = FfCommand {
            kind: 0,
            identity,
            status_version: 0,
            generations,
            candidate_handle: 0,
            observation_version: 0,
        };
        assert_eq!(
            unsafe { ff_session_apply_command(session, 1, &enable, &mut output) },
            OK
        );
        let initial_candidate = FfCandidate {
            handle: 10,
            continuity_token: 100,
            identity,
            observation_version: 1,
            generations,
            observed_at_ms: 2,
            expires_at_ms: 12,
            rect: FfRect {
                x: 0.2,
                y: 0.2,
                width: 0.2,
                height: 0.3,
            },
        };
        assert_eq!(
            unsafe { ff_session_publish_candidate(session, 2, &initial_candidate, &mut output) },
            OK
        );
        let select = FfCommand {
            kind: 4,
            identity,
            status_version: output.status_version,
            generations,
            candidate_handle: initial_candidate.handle,
            observation_version: initial_candidate.observation_version,
        };
        assert_eq!(
            unsafe { ff_session_apply_command(session, 3, &select, &mut output) },
            OK
        );
        assert_eq!(output.state, 3);
        let held_crop = output.crop;
        let mut malformed = initial_candidate;
        malformed.handle = 11;
        malformed.observation_version = 2;
        malformed.observed_at_ms = 113;
        malformed.expires_at_ms = 123;
        malformed.rect.x = f32::NAN;
        assert_eq!(
            unsafe { ff_session_publish_candidate(session, 113, &malformed, &mut output) },
            OK
        );

        assert_eq!(output.command_result, 2);
        assert_eq!(output.rejection_reason, 4);
        assert_eq!(output.reason, 7);
        assert_eq!(output.state, 4);
        assert_eq!(output.has_selected, 0);
        assert_eq!(output.crop.x, held_crop.x);
        assert_eq!(output.crop.y, held_crop.y);
        assert_eq!(output.crop.width, held_crop.width);
        assert_eq!(output.crop.height, held_crop.height);
        assert_eq!(output.generations.source, generations.source);
        assert_eq!(output.generations.session, generations.session);
        assert_eq!(output.generations.stream, generations.stream);
        assert_eq!(output.generations.geometry, generations.geometry);
        assert_eq!(output.generations.tracker, generations.tracker);

        unsafe { ff_session_destroy(session) };
    }

    #[test]
    fn c_abi_records_keep_the_versioned_layout() {
        use std::mem::{align_of, offset_of, size_of};

        assert_eq!(size_of::<FfGenerations>(), 40);
        assert_eq!(size_of::<FfIdentity>(), 16);
        assert_eq!(size_of::<FfRect>(), 16);
        assert_eq!(size_of::<FfSessionConfig>(), 72);
        assert_eq!(size_of::<FfCandidate>(), 112);
        assert_eq!(size_of::<FfCommand>(), 88);
        assert_eq!(size_of::<FfLifecycle>(), 72);
        assert_eq!(size_of::<FfSnapshot>(), 152);
        assert_eq!(align_of::<FfGenerations>(), 8);
        assert_eq!(align_of::<FfIdentity>(), 8);
        assert_eq!(align_of::<FfSnapshot>(), 8);
        assert_eq!(offset_of!(FfCandidate, generations), 40);
        assert_eq!(offset_of!(FfCandidate, rect), 96);
        assert_eq!(offset_of!(FfSessionConfig, dead_zone_fraction), 60);
        assert_eq!(offset_of!(FfSessionConfig, zoom_smoothing), 64);
        assert_eq!(offset_of!(FfCommand, generations), 32);
        assert_eq!(offset_of!(FfLifecycle, generations), 24);
        assert_eq!(offset_of!(FfSnapshot, crop), 76);
        assert_eq!(offset_of!(FfSnapshot, generations), 112);
    }
}
