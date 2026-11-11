use faithfulframe_core::framing::FramingConfig;
use faithfulframe_core::{
    Candidate, Command, CommandEnvelope, CommandResult, Config, CropAction, Event, Generations,
    Identity, Lifecycle, Reason, Rect, Snapshot, State, TrackingEngine,
};
use serde::{Deserialize, Serialize};
use std::fs;
use std::path::PathBuf;

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct Fixture {
    schema_version: u32,
    name: String,
    identity: FixtureIdentity,
    generations: FixtureGenerations,
    session: FixtureSessionConfig,
    framing: FixtureFramingConfig,
    events: Vec<FixtureStep>,
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct FixtureIdentity {
    source: u64,
    session: u64,
}

impl From<&FixtureIdentity> for Identity {
    fn from(value: &FixtureIdentity) -> Self {
        Self {
            source: value.source,
            session: value.session,
        }
    }
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct FixtureGenerations {
    source: u64,
    session: u64,
    stream: u64,
    geometry: u64,
    tracker: u64,
}

impl From<&FixtureGenerations> for Generations {
    fn from(value: &FixtureGenerations) -> Self {
        Self {
            source: value.source,
            session: value.session,
            stream: value.stream,
            geometry: value.geometry,
            tracker: value.tracker,
        }
    }
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct FixtureSessionConfig {
    loss_grace_ms: u64,
    max_evidence_age_ms: u64,
    max_candidates: usize,
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct FixtureFramingConfig {
    source_aspect: f32,
    output_aspect: f32,
    target_coverage: f32,
    min_zoom: f32,
    max_zoom: f32,
    smoothing: f32,
    dead_zone_fraction: f32,
    max_pan_step: f32,
    max_zoom_step: f32,
}

impl From<&FixtureFramingConfig> for FramingConfig {
    fn from(value: &FixtureFramingConfig) -> Self {
        Self {
            source_aspect: value.source_aspect,
            output_aspect: value.output_aspect,
            target_coverage: value.target_coverage,
            min_zoom: value.min_zoom,
            max_zoom: value.max_zoom,
            smoothing: value.smoothing,
            dead_zone_fraction: value.dead_zone_fraction,
            max_pan_step: value.max_pan_step,
            max_zoom_step: value.max_zoom_step,
        }
    }
}

#[derive(Clone, Copy, Debug, Deserialize)]
#[serde(rename_all = "snake_case")]
enum FixtureEventKind {
    Command,
    Candidate,
    Tick,
    Lifecycle,
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct FixtureEvent {
    kind: FixtureEventKind,
    at_ms: u64,
    queue_order: u64,
    identity: Option<FixtureIdentity>,
    generations: Option<FixtureGenerations>,
    status_version: Option<u64>,
    command: Option<FixtureCommand>,
    handle: Option<u64>,
    continuity: Option<u64>,
    observation_version: Option<u64>,
    observed_at_ms: Option<u64>,
    expires_at_ms: Option<u64>,
    rect: Option<FixtureRect>,
    event: Option<FixtureLifecycle>,
}

impl FixtureEvent {
    fn assert_valid_shape(&self) {
        let no_candidates = self.handle.is_none()
            && self.continuity.is_none()
            && self.observation_version.is_none()
            && self.observed_at_ms.is_none()
            && self.expires_at_ms.is_none()
            && self.rect.is_none();
        match self.kind {
            FixtureEventKind::Command => assert!(
                self.identity.is_some()
                    && self.generations.is_some()
                    && self.status_version.is_some()
                    && self.command.is_some()
                    && no_candidates
                    && self.event.is_none(),
                "command fixture row has missing or unrelated fields"
            ),
            FixtureEventKind::Candidate => assert!(
                self.identity.is_some()
                    && self.generations.is_some()
                    && self.handle.is_some()
                    && self.continuity.is_some()
                    && self.observation_version.is_some()
                    && self.observed_at_ms.is_some()
                    && self.expires_at_ms.is_some()
                    && self.rect.is_some()
                    && self.status_version.is_none()
                    && self.command.is_none()
                    && self.event.is_none(),
                "candidate fixture row has missing or unrelated fields"
            ),
            FixtureEventKind::Tick => assert!(
                self.identity.is_none()
                    && self.generations.is_none()
                    && self.status_version.is_none()
                    && self.command.is_none()
                    && no_candidates
                    && self.event.is_none(),
                "tick fixture row has unrelated fields"
            ),
            FixtureEventKind::Lifecycle => assert!(
                self.identity.is_some()
                    && self.generations.is_some()
                    && self.status_version.is_none()
                    && self.command.is_none()
                    && no_candidates
                    && self.event.is_some(),
                "lifecycle fixture row has missing or unrelated fields"
            ),
        }
    }
}

#[derive(Debug, Deserialize)]
#[serde(tag = "action", rename_all = "snake_case", deny_unknown_fields)]
enum FixtureCommand {
    EnableAutoFollow,
    Freeze,
    Resume,
    ManualOverride,
    ResetToWide,
    SelectCandidate {
        handle: u64,
        observation_version: u64,
    },
}

#[derive(Debug, Deserialize)]
#[serde(tag = "event", rename_all = "snake_case", deny_unknown_fields)]
enum FixtureLifecycle {
    SceneInactive,
    SceneActive,
    CameraDisconnected,
    CameraRestored,
    SourceRebound,
    GeometryChanged {
        source_width: u32,
        source_height: u32,
    },
    TrackerChanged,
    FramingConfigChanged,
    CandidatePolicyChanged,
    AdapterDestroyed,
    SourceDestroyed,
    ObsShutdown,
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct FixtureRect {
    x: f32,
    y: f32,
    width: f32,
    height: f32,
}

impl From<&FixtureRect> for Rect {
    fn from(value: &FixtureRect) -> Self {
        Self {
            x: value.x,
            y: value.y,
            width: value.width,
            height: value.height,
        }
    }
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct FixtureStep {
    #[serde(flatten)]
    event: FixtureEvent,
    expected: TraceOutput,
}

#[derive(Clone, Debug, Deserialize, Serialize, PartialEq, Eq)]
#[serde(deny_unknown_fields)]
struct FixtureGenerationsTrace {
    source: u64,
    session: u64,
    stream: u64,
    geometry: u64,
    tracker: u64,
}

impl From<Generations> for FixtureGenerationsTrace {
    fn from(value: Generations) -> Self {
        Self {
            source: value.source,
            session: value.session,
            stream: value.stream,
            geometry: value.geometry,
            tracker: value.tracker,
        }
    }
}

#[derive(Clone, Debug, Deserialize, Serialize, PartialEq, Eq)]
#[serde(deny_unknown_fields)]
struct GenerationDelta {
    source: u64,
    session: u64,
    stream: u64,
    geometry: u64,
    tracker: u64,
}

#[derive(Clone, Debug, Deserialize, Serialize, PartialEq, Eq)]
#[serde(deny_unknown_fields)]
struct TraceOutput {
    previous_state: String,
    transitioned: bool,
    state: String,
    reason: String,
    command_result: String,
    selected_handle: Option<u64>,
    selected_rect_microunits: Option<[u32; 4]>,
    crop_action: String,
    source_crop_microunits: [u32; 4],
    output_viewport_microunits: [u32; 4],
    status_version: u64,
    observation_version: u64,
    generations: FixtureGenerationsTrace,
    generation_delta: GenerationDelta,
    closed: bool,
}

#[derive(Serialize)]
struct CanonicalTraceRow {
    at_ms: u64,
    queue_order: u64,
    event_kind: &'static str,
    output: TraceOutput,
}

fn event_metadata(event: &FixtureEvent) -> (u64, u64, &'static str) {
    let kind = match event.kind {
        FixtureEventKind::Command => "command",
        FixtureEventKind::Candidate => "candidate",
        FixtureEventKind::Tick => "tick",
        FixtureEventKind::Lifecycle => "lifecycle",
    };
    (event.at_ms, event.queue_order, kind)
}

fn to_event(event: &FixtureEvent) -> Event {
    match event.kind {
        FixtureEventKind::Command => Event::Command {
            at: event.at_ms,
            envelope: CommandEnvelope {
                identity: event.identity.as_ref().expect("command identity").into(),
                status_version: event.status_version.expect("command status version"),
                generations: event
                    .generations
                    .as_ref()
                    .expect("command generations")
                    .into(),
                command: match event.command.as_ref().expect("command action") {
                    FixtureCommand::EnableAutoFollow => Command::EnableAutoFollow,
                    FixtureCommand::Freeze => Command::Freeze,
                    FixtureCommand::Resume => Command::Resume,
                    FixtureCommand::ManualOverride => Command::ManualOverride,
                    FixtureCommand::ResetToWide => Command::ResetToWide,
                    FixtureCommand::SelectCandidate {
                        handle,
                        observation_version,
                    } => Command::SelectCandidate {
                        handle: *handle,
                        observation_version: *observation_version,
                    },
                },
            },
        },
        FixtureEventKind::Candidate => Event::Candidate {
            at: event.at_ms,
            candidate: Candidate {
                handle: event.handle.expect("candidate handle"),
                continuity: event.continuity.expect("candidate continuity token"),
                identity: event.identity.as_ref().expect("candidate identity").into(),
                observation_version: event
                    .observation_version
                    .expect("candidate observation version"),
                generations: event
                    .generations
                    .as_ref()
                    .expect("candidate generations")
                    .into(),
                observed_at: event.observed_at_ms.expect("candidate observed_at_ms"),
                expires_at: event.expires_at_ms.expect("candidate expires_at_ms"),
                rect: event.rect.as_ref().expect("candidate rect").into(),
            },
        },
        FixtureEventKind::Tick => Event::Tick { at: event.at_ms },
        FixtureEventKind::Lifecycle => {
            let generations = event.generations.as_ref().expect("lifecycle generations");
            Event::Lifecycle {
                at: event.at_ms,
                identity: event.identity.as_ref().expect("lifecycle identity").into(),
                event: match event.event.as_ref().expect("lifecycle event") {
                    FixtureLifecycle::SceneInactive => Lifecycle::SceneInactive {
                        generations: generations.into(),
                    },
                    FixtureLifecycle::SceneActive => Lifecycle::SceneActive {
                        generations: generations.into(),
                    },
                    FixtureLifecycle::CameraDisconnected => Lifecycle::CameraDisconnected {
                        generations: generations.into(),
                    },
                    FixtureLifecycle::CameraRestored => Lifecycle::CameraRestored {
                        generations: generations.into(),
                    },
                    FixtureLifecycle::SourceRebound => Lifecycle::SourceRebound {
                        generations: generations.into(),
                    },
                    FixtureLifecycle::GeometryChanged {
                        source_width,
                        source_height,
                    } => Lifecycle::GeometryChanged {
                        generations: generations.into(),
                        source_width: *source_width,
                        source_height: *source_height,
                    },
                    FixtureLifecycle::TrackerChanged => Lifecycle::TrackerChanged {
                        generations: generations.into(),
                    },
                    FixtureLifecycle::FramingConfigChanged => Lifecycle::FramingConfigChanged {
                        generations: generations.into(),
                    },
                    FixtureLifecycle::CandidatePolicyChanged => Lifecycle::CandidatePolicyChanged {
                        generations: generations.into(),
                    },
                    FixtureLifecycle::AdapterDestroyed => Lifecycle::AdapterDestroyed {
                        generations: generations.into(),
                    },
                    FixtureLifecycle::SourceDestroyed => Lifecycle::SourceDestroyed {
                        generations: generations.into(),
                    },
                    FixtureLifecycle::ObsShutdown => Lifecycle::ObsShutdown {
                        generations: generations.into(),
                    },
                },
            }
        }
    }
}

fn state_name(state: State) -> &'static str {
    match state {
        State::Frozen => "frozen",
        State::Manual => "manual",
        State::Searching => "searching",
        State::Locked => "locked",
        State::Lost => "lost",
    }
}

fn reason_name(reason: Reason) -> &'static str {
    match reason {
        Reason::Startup => "startup",
        Reason::OperatorFreeze => "operator_freeze",
        Reason::ManualOverride => "manual_override",
        Reason::AwaitingSelection => "awaiting_selection",
        Reason::SelectedByOperator => "selected_by_operator",
        Reason::ContinuityRecovered => "continuity_recovered",
        Reason::TargetEvidenceStale => "target_evidence_stale",
        Reason::TargetLossDeadlineExpired => "target_loss_deadline_expired",
        Reason::ResetWide => "reset_wide",
        Reason::LifecycleInvalidated => "lifecycle_invalidated",
        Reason::StaleCommand => "stale_command",
        Reason::InvalidCommand => "invalid_command",
        Reason::NonMonotonicEvent => "non_monotonic_event",
        Reason::InvalidCandidate => "invalid_candidate",
        Reason::StaleLifecycle => "stale_lifecycle",
        Reason::InvalidLifecycle => "invalid_lifecycle",
        Reason::TrackerIdentityExpired => "tracker_identity_expired",
        Reason::SessionClosing => "session_closing",
        Reason::SessionClosed => "session_closed",
        Reason::FramingFailure => "framing_failure",
    }
}

fn crop_action_name(action: CropAction) -> &'static str {
    match action {
        CropAction::Hold => "hold",
        CropAction::Wide => "wide",
        CropAction::Track => "track",
        CropAction::None => "none",
    }
}

fn command_result_name(result: CommandResult) -> String {
    match result {
        CommandResult::Applied => "applied".to_owned(),
        CommandResult::NotACommand => "not_a_command".to_owned(),
        CommandResult::Rejected(reason) => format!("rejected:{}", reason_name(reason)),
    }
}

fn rect_microunits(rect: Rect) -> [u32; 4] {
    [rect.x, rect.y, rect.width, rect.height]
        .map(|value| (value.clamp(0.0, 1.0) * 1_000_000.0).round() as u32)
}

fn generation_delta(previous: Generations, current: Generations) -> GenerationDelta {
    GenerationDelta {
        source: current.source.saturating_sub(previous.source),
        session: current.session.saturating_sub(previous.session),
        stream: current.stream.saturating_sub(previous.stream),
        geometry: current.geometry.saturating_sub(previous.geometry),
        tracker: current.tracker.saturating_sub(previous.tracker),
    }
}

fn trace_output(previous: Snapshot, current: Snapshot) -> TraceOutput {
    TraceOutput {
        previous_state: state_name(previous.state).to_owned(),
        transitioned: previous.state != current.state,
        state: state_name(current.state).to_owned(),
        reason: reason_name(current.reason).to_owned(),
        command_result: command_result_name(current.command_result),
        selected_handle: current.selected,
        selected_rect_microunits: current.selected_rect.map(rect_microunits),
        crop_action: crop_action_name(current.crop_action).to_owned(),
        source_crop_microunits: rect_microunits(current.crop),
        output_viewport_microunits: rect_microunits(current.viewport),
        status_version: current.status_version,
        observation_version: current.observation_version,
        generations: current.generations.into(),
        generation_delta: generation_delta(previous.generations, current.generations),
        closed: current.closed,
    }
}

fn fixture_paths() -> Vec<PathBuf> {
    let root = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("tests/fixtures");
    let mut paths = fs::read_dir(root)
        .expect("fixture directory must exist")
        .map(|entry| entry.expect("fixture entry must be readable").path())
        .filter(|path| {
            path.extension()
                .is_some_and(|extension| extension == "json")
        })
        .collect::<Vec<_>>();
    paths.sort();
    paths
}

fn parse_fixture(path: &PathBuf) -> Fixture {
    let contents = fs::read_to_string(path).expect("fixture must be readable");
    let fixture: Fixture = serde_json::from_str(&contents)
        .unwrap_or_else(|error| panic!("{}: {error}", path.display()));
    assert_eq!(fixture.schema_version, 1, "{}", path.display());
    fixture
}

/// The fixture array is the serialized executor's FIFO order. `queue_order` is
/// unique and increasing; monotonic time may repeat but never decreases. At a
/// shared timestamp the lower queue order wins, so a timer tick and an operator
/// command have an explicit order. Expiry is checked by policy at the event's
/// timestamp (`expires_at >= at` is valid), and timer deadlines are explicit
/// `tick` rows rather than implicit wall-clock work.
fn replay(fixture: &Fixture) -> Vec<CanonicalTraceRow> {
    let mut engine = TrackingEngine::new(
        Config {
            loss_grace_ms: fixture.session.loss_grace_ms,
            max_evidence_age_ms: fixture.session.max_evidence_age_ms,
            safe_wide: Rect::full_frame(),
            max_candidates: fixture.session.max_candidates,
        },
        FramingConfig::from(&fixture.framing),
        Identity::from(&fixture.identity),
        Generations::from(&fixture.generations),
    )
    .unwrap_or_else(|error| panic!("{}: invalid initial config: {error:?}", fixture.name));

    let mut previous_order = 0;
    let mut previous_time = 0;
    fixture
        .events
        .iter()
        .map(|step| {
            let (at_ms, queue_order, event_kind) = event_metadata(&step.event);
            step.event.assert_valid_shape();
            assert!(
                queue_order > previous_order,
                "{}: queue_order must increase",
                fixture.name
            );
            assert!(
                at_ms >= previous_time,
                "{}: fixture timestamps must be monotonic",
                fixture.name
            );
            previous_order = queue_order;
            previous_time = at_ms;
            let before = engine.snapshot();
            let after = engine.apply(to_event(&step.event));
            CanonicalTraceRow {
                at_ms,
                queue_order,
                event_kind,
                output: trace_output(before, after),
            }
        })
        .collect()
}

#[test]
fn deterministic_fixtures_replay_to_canonical_traces() {
    for path in fixture_paths() {
        let fixture = parse_fixture(&path);
        assert!(
            !fixture.events.is_empty(),
            "{} must contain events",
            fixture.name
        );
        let actual = replay(&fixture);
        let expected = fixture
            .events
            .iter()
            .map(|step| {
                let (at_ms, queue_order, event_kind) = event_metadata(&step.event);
                CanonicalTraceRow {
                    at_ms,
                    queue_order,
                    event_kind,
                    output: step.expected.clone(),
                }
            })
            .collect::<Vec<_>>();
        for (actual_row, expected_row) in actual.iter().zip(&expected) {
            assert_eq!(
                serde_json::to_string_pretty(actual_row).unwrap(),
                serde_json::to_string_pretty(expected_row).unwrap(),
                "canonical replay row mismatch for {} at queue order {}",
                fixture.name,
                actual_row.queue_order
            );
        }
        assert_eq!(
            serde_json::to_string_pretty(&actual).unwrap(),
            serde_json::to_string_pretty(&expected).unwrap(),
            "canonical replay trace mismatch for {}",
            fixture.name
        );
    }
}
