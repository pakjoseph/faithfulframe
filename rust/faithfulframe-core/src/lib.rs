//! Deterministic, OBS-independent tracking safety policy.
//!
//! This crate deliberately owns no threads and accepts only monotonic event
//! times. Its caller serializes events onto one executor.

pub mod framing;

use std::collections::BTreeMap;

pub type Millis = u64;
pub type CandidateHandle = u64;
pub type ContinuityToken = u64;

/// Opaque identities are stable for one adapter/session lifetime. Generation
/// counters describe changes within those identities and are not substitutes.
#[derive(Clone, Copy, Debug, Eq, PartialEq, Ord, PartialOrd)]
pub struct Identity {
    pub source: u64,
    pub session: u64,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq, Ord, PartialOrd)]
pub struct Generations {
    pub source: u64,
    pub session: u64,
    pub stream: u64,
    pub geometry: u64,
    pub tracker: u64,
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Rect {
    pub x: f32,
    pub y: f32,
    pub width: f32,
    pub height: f32,
}

impl Rect {
    pub fn normalized(x: f32, y: f32, width: f32, height: f32) -> Result<Self, GeometryError> {
        let rect = Self {
            x,
            y,
            width,
            height,
        };
        if !x.is_finite() || !y.is_finite() || !width.is_finite() || !height.is_finite() {
            return Err(GeometryError::NonFinite);
        }
        if width <= 0.0 || height <= 0.0 {
            return Err(GeometryError::NonPositiveExtent);
        }
        if x < 0.0 || y < 0.0 || x + width > 1.0 || y + height > 1.0 {
            return Err(GeometryError::OutOfBounds);
        }
        Ok(rect)
    }

    pub const fn full_frame() -> Self {
        Self {
            x: 0.0,
            y: 0.0,
            width: 1.0,
            height: 1.0,
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum GeometryError {
    NonFinite,
    NonPositiveExtent,
    OutOfBounds,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum SessionConfigError {
    InvalidIdentity,
    InvalidSafeWide(GeometryError),
    SafeWideMustBeFullFrame,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum State {
    Frozen,
    Manual,
    Searching,
    Locked,
    Lost,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Reason {
    Startup,
    OperatorFreeze,
    ManualOverride,
    AwaitingSelection,
    SelectedByOperator,
    ContinuityRecovered,
    TargetEvidenceStale,
    TargetLossDeadlineExpired,
    ResetWide,
    LifecycleInvalidated,
    StaleCommand,
    InvalidCommand,
    NonMonotonicEvent,
    InvalidCandidate,
    StaleLifecycle,
    InvalidLifecycle,
    TrackerIdentityExpired,
    SessionClosing,
    SessionClosed,
    FramingFailure,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum CropAction {
    Hold,
    Wide,
    Track,
    None,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Command {
    EnableAutoFollow,
    Freeze,
    Resume,
    ManualOverride,
    SelectCandidate {
        handle: CandidateHandle,
        observation_version: u64,
    },
    ResetToWide,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct CommandEnvelope {
    pub identity: Identity,
    /// Target selection is fenced by this preview/status version. Other
    /// controls are validated against the current state and generations so
    /// harmless status refreshes cannot discard an operator safety action.
    pub status_version: u64,
    pub generations: Generations,
    pub command: Command,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Lifecycle {
    SceneInactive {
        generations: Generations,
    },
    SceneActive {
        generations: Generations,
    },
    CameraDisconnected {
        generations: Generations,
    },
    CameraRestored {
        generations: Generations,
    },
    SourceRebound {
        generations: Generations,
    },
    GeometryChanged {
        generations: Generations,
        source_width: u32,
        source_height: u32,
    },
    TrackerChanged {
        generations: Generations,
    },
    FramingConfigChanged {
        generations: Generations,
    },
    CandidatePolicyChanged {
        generations: Generations,
    },
    AdapterDestroyed {
        generations: Generations,
    },
    SourceDestroyed {
        generations: Generations,
    },
    ObsShutdown {
        generations: Generations,
    },
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Candidate {
    /// Monotonically allocated within a session. Issued handles are never reused.
    pub handle: CandidateHandle,
    pub continuity: ContinuityToken,
    pub identity: Identity,
    pub observation_version: u64,
    pub generations: Generations,
    pub observed_at: Millis,
    pub expires_at: Millis,
    pub rect: Rect,
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Event {
    Command {
        at: Millis,
        envelope: CommandEnvelope,
    },
    Candidate {
        at: Millis,
        candidate: Candidate,
    },
    Tick {
        at: Millis,
    },
    Lifecycle {
        at: Millis,
        identity: Identity,
        event: Lifecycle,
    },
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum CommandResult {
    Applied,
    Rejected(Reason),
    NotACommand,
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Snapshot {
    pub state: State,
    pub reason: Reason,
    pub identity: Identity,
    pub generations: Generations,
    pub status_version: u64,
    pub observation_version: u64,
    pub selected: Option<CandidateHandle>,
    pub selected_rect: Option<Rect>,
    pub crop_action: CropAction,
    pub crop: Rect,
    pub viewport: Rect,
    pub loss_deadline: Option<Millis>,
    pub closed: bool,
    pub command_result: CommandResult,
}

#[derive(Clone, Copy, Debug)]
pub struct Config {
    pub loss_grace_ms: Millis,
    pub max_evidence_age_ms: Millis,
    pub safe_wide: Rect,
    pub max_candidates: usize,
}

impl Default for Config {
    fn default() -> Self {
        Self {
            loss_grace_ms: 5_000,
            max_evidence_age_ms: 500,
            safe_wide: Rect::full_frame(),
            max_candidates: 8,
        }
    }
}

#[derive(Clone, Copy, Debug)]
struct SelectedTarget {
    handle: CandidateHandle,
    continuity: ContinuityToken,
    last_seen: Millis,
    rect: Rect,
}

/// A deterministic single-threaded safety policy. Callers must serialize all
/// calls to `apply`; this type has no implicit timing or background work.
pub struct TrackingSession {
    config: Config,
    identity: Identity,
    generations: Generations,
    state: State,
    reason: Reason,
    selected: Option<SelectedTarget>,
    latest: BTreeMap<CandidateHandle, Candidate>,
    highest_candidate_handle: CandidateHandle,
    held_crop: Rect,
    crop_action: CropAction,
    loss_started_at: Option<Millis>,
    loss_deadline: Option<Millis>,
    status_version: u64,
    observation_version: u64,
    last_at: Millis,
    closed: bool,
}

impl TrackingSession {
    pub fn new(
        config: Config,
        identity: Identity,
        generations: Generations,
    ) -> Result<Self, SessionConfigError> {
        if identity.source == 0 || identity.session == 0 {
            return Err(SessionConfigError::InvalidIdentity);
        }
        let safe_wide = Rect::normalized(
            config.safe_wide.x,
            config.safe_wide.y,
            config.safe_wide.width,
            config.safe_wide.height,
        )
        .map_err(SessionConfigError::InvalidSafeWide)?;
        if safe_wide != Rect::full_frame() {
            return Err(SessionConfigError::SafeWideMustBeFullFrame);
        }
        Ok(Self {
            config: Config {
                safe_wide,
                max_candidates: config.max_candidates.max(1),
                ..config
            },
            identity,
            generations,
            state: State::Manual,
            reason: Reason::Startup,
            selected: None,
            latest: BTreeMap::new(),
            highest_candidate_handle: 0,
            held_crop: safe_wide,
            crop_action: CropAction::None,
            loss_started_at: None,
            loss_deadline: None,
            status_version: 0,
            observation_version: 0,
            last_at: 0,
            closed: false,
        })
    }

    pub fn snapshot(&self) -> Snapshot {
        Snapshot {
            state: self.state,
            reason: self.reason,
            identity: self.identity,
            generations: self.generations,
            status_version: self.status_version,
            observation_version: self.observation_version,
            selected: self.selected.map(|target| target.handle),
            selected_rect: self.selected.map(|target| target.rect),
            crop_action: self.crop_action,
            crop: self.held_crop,
            viewport: Rect::full_frame(),
            loss_deadline: self.loss_deadline,
            closed: self.closed,
            command_result: CommandResult::NotACommand,
        }
    }

    pub fn apply(&mut self, event: Event) -> Snapshot {
        let at = event_time(event);
        if self.closed {
            return self.reject(Reason::SessionClosed);
        }
        if at < self.last_at {
            return self.reject(Reason::NonMonotonicEvent);
        }

        // Deadlines are authoritative at their monotonic timestamp, even if
        // the executor receives a candidate, command, or lifecycle event
        // before the next periodic Tick. This prevents queued detector results
        // from reviving a session after its grace period.
        let timer_advanced = self.advance_due_timers(at);
        let tick_event = matches!(&event, Event::Tick { .. });

        let result = match event {
            Event::Command { envelope, .. } => {
                if envelope.identity != self.identity || envelope.generations != self.generations {
                    self.reject_command(Reason::StaleCommand)
                } else {
                    self.apply_command(envelope.command, envelope.status_version, at)
                }
            }
            Event::Candidate { at, candidate } => self.apply_candidate(candidate, at),
            Event::Tick { .. } => CommandResult::NotACommand,
            Event::Lifecycle {
                identity, event, ..
            } => {
                if identity != self.identity {
                    self.reject_command(Reason::StaleLifecycle)
                } else {
                    self.apply_lifecycle(event)
                }
            }
        };
        if !matches!(result, CommandResult::Rejected(_)) {
            self.last_at = at;
            if !timer_advanced || !tick_event {
                self.status_version = self.status_version.saturating_add(1);
            }
        }
        let mut snapshot = self.snapshot();
        snapshot.command_result = result;
        snapshot
    }

    fn apply_command(
        &mut self,
        command: Command,
        status_version: u64,
        at: Millis,
    ) -> CommandResult {
        match command {
            Command::Freeze if self.state != State::Frozen => {
                self.state = State::Frozen;
                self.reason = Reason::OperatorFreeze;
                self.crop_action = CropAction::Hold;
                self.loss_started_at = None;
                self.loss_deadline = None;
                CommandResult::Applied
            }
            Command::Resume if self.state == State::Frozen => {
                self.state = State::Manual;
                self.reason = Reason::AwaitingSelection;
                self.crop_action = CropAction::None;
                CommandResult::Applied
            }
            Command::ManualOverride if self.state != State::Frozen => {
                self.state = State::Manual;
                self.reason = Reason::ManualOverride;
                self.selected = None;
                self.loss_started_at = None;
                self.loss_deadline = None;
                self.crop_action = CropAction::None;
                CommandResult::Applied
            }
            Command::EnableAutoFollow if matches!(self.state, State::Manual | State::Lost) => {
                self.state = State::Searching;
                self.reason = Reason::AwaitingSelection;
                self.selected = None;
                self.loss_started_at = Some(at);
                self.loss_deadline = Some(at.saturating_add(self.config.loss_grace_ms));
                self.crop_action = CropAction::Hold;
                CommandResult::Applied
            }
            Command::ResetToWide => {
                if self.state != State::Frozen {
                    self.state = State::Manual;
                }
                self.selected = None;
                self.loss_started_at = None;
                self.loss_deadline = None;
                self.held_crop = self.config.safe_wide;
                self.crop_action = CropAction::Wide;
                self.reason = Reason::ResetWide;
                CommandResult::Applied
            }
            Command::SelectCandidate {
                handle,
                observation_version,
            } if matches!(self.state, State::Manual | State::Searching | State::Lost)
                && status_version == self.status_version
                && observation_version == self.observation_version =>
            {
                if let Some(candidate) = self.latest.get(&handle).copied()
                    && candidate.expires_at >= at
                    && candidate.observed_at <= at
                    && at.saturating_sub(candidate.observed_at) <= self.config.max_evidence_age_ms
                {
                    self.selected = Some(SelectedTarget {
                        handle,
                        continuity: candidate.continuity,
                        last_seen: candidate.observed_at,
                        rect: candidate.rect,
                    });
                    self.state = State::Locked;
                    self.reason = Reason::SelectedByOperator;
                    self.crop_action = CropAction::Track;
                    self.loss_started_at = None;
                    self.loss_deadline = None;
                    CommandResult::Applied
                } else {
                    self.reject_command(Reason::StaleCommand)
                }
            }
            Command::SelectCandidate { .. } => self.reject_command(Reason::StaleCommand),
            _ => self.reject_command(Reason::InvalidCommand),
        }
    }

    fn apply_candidate(&mut self, candidate: Candidate, at: Millis) -> CommandResult {
        if candidate.handle == 0
            || candidate.continuity == 0
            || candidate.observation_version == 0
            || candidate.identity != self.identity
            || candidate.generations != self.generations
            || candidate.observed_at > at
            || candidate.expires_at < at
            || at.saturating_sub(candidate.observed_at) > self.config.max_evidence_age_ms
        {
            return self.reject_command(Reason::InvalidCandidate);
        }
        if candidate.observation_version < self.observation_version {
            return self.reject_command(Reason::InvalidCandidate);
        }
        if candidate.expires_at < candidate.observed_at {
            return self.reject_command(Reason::InvalidCandidate);
        }
        if Rect::normalized(
            candidate.rect.x,
            candidate.rect.y,
            candidate.rect.width,
            candidate.rect.height,
        )
        .is_err()
        {
            return self.reject_command(Reason::InvalidCandidate);
        }
        if let Some(previous) = self.latest.get(&candidate.handle) {
            if previous.continuity != candidate.continuity {
                return self.reject_command(Reason::InvalidCandidate);
            }
        } else if candidate.handle <= self.highest_candidate_handle {
            // A bounded high-water mark rejects recycled/late handles without
            // retaining every candidate ever seen during a long session.
            return self.reject_command(Reason::InvalidCandidate);
        }
        if self
            .latest
            .get(&candidate.handle)
            .is_some_and(|previous| candidate.observed_at < previous.observed_at)
        {
            return self.reject_command(Reason::InvalidCandidate);
        }
        self.highest_candidate_handle = self.highest_candidate_handle.max(candidate.handle);
        self.latest.insert(candidate.handle, candidate);
        while self.latest.len() > self.config.max_candidates {
            if let Some(oldest) = self.latest.keys().next().copied() {
                self.latest.remove(&oldest);
            }
        }
        self.observation_version = self.observation_version.max(candidate.observation_version);

        if let Some(selected) = self.selected {
            if selected.continuity == candidate.continuity
                && self.state == State::Locked
                && candidate.observed_at >= selected.last_seen
            {
                self.selected = Some(SelectedTarget {
                    handle: candidate.handle,
                    last_seen: candidate.observed_at,
                    rect: candidate.rect,
                    ..selected
                });
                self.crop_action = CropAction::Track;
            } else if selected.continuity == candidate.continuity
                && self.state == State::Searching
                && self
                    .loss_started_at
                    .is_some_and(|started_at| candidate.observed_at >= started_at)
                && self
                    .loss_deadline
                    .is_some_and(|deadline| candidate.observed_at < deadline)
            {
                self.selected = Some(SelectedTarget {
                    handle: candidate.handle,
                    last_seen: candidate.observed_at,
                    rect: candidate.rect,
                    ..selected
                });
                self.state = State::Locked;
                self.reason = Reason::ContinuityRecovered;
                self.crop_action = CropAction::Track;
                self.loss_started_at = None;
                self.loss_deadline = None;
            }
        }
        CommandResult::NotACommand
    }

    fn advance_due_timers(&mut self, at: Millis) -> bool {
        let mut advanced = false;
        if self.state == State::Locked
            && self.selected.is_some_and(|target| {
                at.saturating_sub(target.last_seen) > self.config.max_evidence_age_ms
            })
        {
            let last_seen = self.selected.map_or(at, |target| target.last_seen);
            let stale_at = last_seen
                .saturating_add(self.config.max_evidence_age_ms)
                .saturating_add(1);
            self.state = State::Searching;
            self.reason = Reason::TargetEvidenceStale;
            self.crop_action = CropAction::Hold;
            self.loss_started_at = Some(stale_at);
            self.loss_deadline = Some(stale_at.saturating_add(self.config.loss_grace_ms));
            self.last_at = stale_at;
            self.status_version = self.status_version.saturating_add(1);
            advanced = true;
        }
        if self.state == State::Searching
            && self.loss_deadline.is_some_and(|deadline| at >= deadline)
        {
            let deadline = self.loss_deadline.unwrap_or(at);
            self.state = State::Lost;
            self.reason = Reason::TargetLossDeadlineExpired;
            self.crop_action = CropAction::Hold;
            self.loss_started_at = None;
            self.loss_deadline = None;
            self.selected = None;
            self.last_at = deadline;
            self.status_version = self.status_version.saturating_add(1);
            advanced = true;
        }
        advanced
    }

    fn apply_lifecycle(&mut self, event: Lifecycle) -> CommandResult {
        if matches!(
            event,
            Lifecycle::GeometryChanged {
                source_width: 0,
                ..
            } | Lifecycle::GeometryChanged {
                source_height: 0,
                ..
            }
        ) {
            return self.reject_command(Reason::InvalidLifecycle);
        }
        let Some(generations) = expected_lifecycle_generations(self.generations, event) else {
            return self.reject_command(Reason::InvalidLifecycle);
        };
        let (reason, state, crop_action, closes_session) = match event {
            Lifecycle::TrackerChanged { .. } => (
                Reason::TrackerIdentityExpired,
                State::Lost,
                CropAction::Hold,
                false,
            ),
            Lifecycle::SourceRebound { .. }
            | Lifecycle::FramingConfigChanged { .. }
            | Lifecycle::CandidatePolicyChanged { .. }
            | Lifecycle::AdapterDestroyed { .. }
            | Lifecycle::SourceDestroyed { .. }
            | Lifecycle::ObsShutdown { .. } => (
                Reason::SessionClosing,
                State::Manual,
                CropAction::Wide,
                true,
            ),
            Lifecycle::SceneInactive { .. }
            | Lifecycle::SceneActive { .. }
            | Lifecycle::CameraDisconnected { .. }
            | Lifecycle::CameraRestored { .. } => (
                Reason::LifecycleInvalidated,
                State::Manual,
                CropAction::Hold,
                false,
            ),
            Lifecycle::GeometryChanged { .. } => (
                Reason::LifecycleInvalidated,
                State::Manual,
                CropAction::Wide,
                false,
            ),
        };
        self.invalidate_to_state(generations, reason, state, crop_action, closes_session);
        CommandResult::NotACommand
    }

    fn invalidate_to_state(
        &mut self,
        generations: Generations,
        reason: Reason,
        state: State,
        crop_action: CropAction,
        closes_session: bool,
    ) {
        self.generations = generations;
        self.selected = None;
        self.latest.clear();
        self.loss_started_at = None;
        self.loss_deadline = None;
        self.state = state;
        self.reason = reason;
        if crop_action == CropAction::Wide {
            self.held_crop = self.config.safe_wide;
        }
        self.crop_action = crop_action;
        self.observation_version = self.observation_version.saturating_add(1);
        self.closed = closes_session;
    }

    fn reject_command(&self, reason: Reason) -> CommandResult {
        CommandResult::Rejected(reason)
    }

    fn reject(&self, reason: Reason) -> Snapshot {
        let mut snapshot = self.snapshot();
        snapshot.command_result = CommandResult::Rejected(reason);
        snapshot
    }

    fn reject_at(&mut self, at: Millis, reason: Reason) -> Snapshot {
        if self.closed {
            return self.reject(Reason::SessionClosed);
        }
        if at < self.last_at {
            return self.reject(Reason::NonMonotonicEvent);
        }
        // Invalid event payloads are strict no-ops, but a genuine deadline at
        // this monotonic time remains an independent authorized transition.
        self.advance_due_timers(at);
        self.reject(reason)
    }

    fn reject_invalid_command_at(
        &mut self,
        at: Millis,
        identity: Identity,
        generations: Generations,
        status_version: u64,
    ) -> Snapshot {
        if self.closed {
            return self.reject(Reason::SessionClosed);
        }
        if at < self.last_at {
            return self.reject(Reason::NonMonotonicEvent);
        }
        self.advance_due_timers(at);
        let reason = if identity != self.identity
            || generations != self.generations
            || status_version != self.status_version
        {
            Reason::StaleCommand
        } else {
            Reason::InvalidCommand
        };
        self.reject(reason)
    }

    fn fail_closed_framing(&mut self) {
        self.state = State::Manual;
        self.reason = Reason::FramingFailure;
        self.selected = None;
        self.latest.clear();
        self.loss_started_at = None;
        self.loss_deadline = None;
        self.held_crop = Rect::full_frame();
        self.crop_action = CropAction::Wide;
        self.status_version = self.status_version.saturating_add(1);
        self.observation_version = self.observation_version.saturating_add(1);
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum EngineConfigError {
    Session(SessionConfigError),
    Framing(framing::FramingError),
}

/// Composes deterministic target policy with independent crop planning. Both
/// are called by the owner's serialized executor; rendering consumes snapshots
/// and never invokes detection or policy work.
pub struct TrackingEngine {
    session: TrackingSession,
    framing_config: framing::FramingConfig,
    framing: framing::FramingController,
    frame_plan: framing::FramePlan,
}

impl TrackingEngine {
    pub fn new(
        session_config: Config,
        framing_config: framing::FramingConfig,
        identity: Identity,
        generations: Generations,
    ) -> Result<Self, EngineConfigError> {
        let session = TrackingSession::new(session_config, identity, generations)
            .map_err(EngineConfigError::Session)?;
        let framing =
            framing::FramingController::new(framing_config).map_err(EngineConfigError::Framing)?;
        let frame_plan = framing.safe_wide();
        Ok(Self {
            session,
            framing_config,
            framing,
            frame_plan,
        })
    }

    pub fn snapshot(&self) -> Snapshot {
        self.with_frame(self.session.snapshot())
    }

    /// Records a syntactically invalid command as a serialized rejection.
    /// Due safety timers still advance independently of that no-op command.
    pub fn reject_invalid_command(
        &mut self,
        at: Millis,
        identity: Identity,
        generations: Generations,
        status_version: u64,
    ) -> Snapshot {
        let rejected =
            self.session
                .reject_invalid_command_at(at, identity, generations, status_version);
        self.with_frame(rejected)
    }

    /// Records an invalid lifecycle tag through the session event path.
    pub fn reject_invalid_lifecycle(&mut self, at: Millis, identity: Identity) -> Snapshot {
        let reason = if identity == self.session.identity {
            Reason::InvalidLifecycle
        } else {
            Reason::StaleLifecycle
        };
        let rejected = self.session.reject_at(at, reason);
        self.with_frame(rejected)
    }

    pub fn apply(&mut self, event: Event) -> Snapshot {
        let at = event_time(event);
        let geometry_update = match event {
            Event::Lifecycle {
                identity,
                event:
                    Lifecycle::GeometryChanged {
                        source_width,
                        source_height,
                        ..
                    },
                ..
            } if identity == self.session.identity => {
                if source_width == 0 || source_height == 0 {
                    let rejected = self.session.reject_at(at, Reason::InvalidLifecycle);
                    return self.with_frame(rejected);
                }
                let mut config = self.framing_config;
                config.source_aspect = source_width as f32 / source_height as f32;
                match framing::FramingController::new(config) {
                    Ok(controller) => Some((config, controller)),
                    Err(_) => {
                        let rejected = self.session.reject_at(at, Reason::InvalidLifecycle);
                        return self.with_frame(rejected);
                    }
                }
            }
            _ => None,
        };

        let policy = self.session.apply(event);
        if matches!(policy.command_result, CommandResult::Rejected(_)) {
            return self.with_frame(policy);
        }

        if let Some((config, framing)) = geometry_update {
            self.framing_config = config;
            self.framing = framing;
            self.frame_plan = self.framing.safe_wide();
        } else {
            match policy.crop_action {
                CropAction::Track => {
                    if let Some(target) = policy.selected_rect {
                        match self.framing.plan(target) {
                            Ok(plan) => self.frame_plan = plan,
                            Err(_) => {
                                self.session.fail_closed_framing();
                                self.framing.reset();
                                self.frame_plan = self.framing.safe_wide();
                            }
                        }
                    }
                }
                CropAction::Wide => {
                    self.framing.reset();
                    self.frame_plan = self.framing.safe_wide();
                }
                CropAction::Hold | CropAction::None => {}
            }
        }
        self.snapshot_with_result(policy.command_result)
    }

    fn with_frame(&self, mut snapshot: Snapshot) -> Snapshot {
        snapshot.crop = self.frame_plan.source_crop;
        snapshot.viewport = self.frame_plan.output_viewport;
        snapshot
    }

    fn snapshot_with_result(&self, result: CommandResult) -> Snapshot {
        let mut snapshot = self.session.snapshot();
        snapshot.command_result = result;
        self.with_frame(snapshot)
    }
}

fn event_time(event: Event) -> Millis {
    match event {
        Event::Command { at, .. } | Event::Tick { at } | Event::Lifecycle { at, .. } => at,
        Event::Candidate { at, .. } => at,
    }
}

fn expected_lifecycle_generations(current: Generations, event: Lifecycle) -> Option<Generations> {
    let (source, session, stream, geometry, tracker) = match event {
        Lifecycle::SceneInactive { .. } | Lifecycle::SceneActive { .. } => {
            (false, true, false, false, true)
        }
        Lifecycle::CameraDisconnected { .. } | Lifecycle::CameraRestored { .. } => {
            (false, false, true, false, true)
        }
        Lifecycle::SourceRebound { .. } => (true, true, true, true, true),
        Lifecycle::GeometryChanged { .. } => (false, false, true, true, true),
        Lifecycle::TrackerChanged { .. } => (false, false, false, false, true),
        Lifecycle::AdapterDestroyed { .. } => (false, true, false, false, true),
        Lifecycle::FramingConfigChanged { .. } => (false, true, false, true, true),
        Lifecycle::CandidatePolicyChanged { .. } => (false, true, false, false, true),
        Lifecycle::SourceDestroyed { .. } | Lifecycle::ObsShutdown { .. } => {
            (true, true, true, false, true)
        }
    };
    let proposed = match event {
        Lifecycle::SceneInactive { generations }
        | Lifecycle::SceneActive { generations }
        | Lifecycle::CameraDisconnected { generations }
        | Lifecycle::CameraRestored { generations }
        | Lifecycle::SourceRebound { generations }
        | Lifecycle::GeometryChanged { generations, .. }
        | Lifecycle::TrackerChanged { generations }
        | Lifecycle::FramingConfigChanged { generations }
        | Lifecycle::CandidatePolicyChanged { generations }
        | Lifecycle::AdapterDestroyed { generations }
        | Lifecycle::SourceDestroyed { generations }
        | Lifecycle::ObsShutdown { generations } => generations,
    };
    let expected = Generations {
        source: increment_generation(current.source, source)?,
        session: increment_generation(current.session, session)?,
        stream: increment_generation(current.stream, stream)?,
        geometry: increment_generation(current.geometry, geometry)?,
        tracker: increment_generation(current.tracker, tracker)?,
    };
    (proposed == expected).then_some(proposed)
}

fn increment_generation(value: u64, increment: bool) -> Option<u64> {
    if increment {
        value.checked_add(1)
    } else {
        Some(value)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn generations() -> Generations {
        Generations {
            source: 1,
            session: 1,
            stream: 1,
            geometry: 1,
            tracker: 1,
        }
    }

    fn identity() -> Identity {
        Identity {
            source: 101,
            session: 201,
        }
    }

    fn rect() -> Rect {
        Rect::normalized(0.2, 0.2, 0.4, 0.4).unwrap()
    }

    fn candidate(handle: u64, token: u64, observation_version: u64, at: Millis) -> Candidate {
        Candidate {
            handle,
            continuity: token,
            identity: identity(),
            observation_version,
            generations: generations(),
            observed_at: at,
            expires_at: at + 10,
            rect: rect(),
        }
    }

    fn send(session: &mut TrackingSession, at: Millis, command: Command) -> Snapshot {
        let snapshot = session.snapshot();
        session.apply(Event::Command {
            at,
            envelope: CommandEnvelope {
                identity: snapshot.identity,
                status_version: snapshot.status_version,
                generations: snapshot.generations,
                command,
            },
        })
    }

    fn observe(session: &mut TrackingSession, candidate: Candidate, at: Millis) -> Snapshot {
        session.apply(Event::Candidate { at, candidate })
    }

    fn session() -> TrackingSession {
        TrackingSession::new(
            Config {
                loss_grace_ms: 100,
                max_evidence_age_ms: 10,
                ..Config::default()
            },
            identity(),
            generations(),
        )
        .unwrap()
    }

    fn generations_after(
        base: Generations,
        source: bool,
        session: bool,
        stream: bool,
        geometry: bool,
        tracker: bool,
    ) -> Generations {
        Generations {
            source: base.source + u64::from(source),
            session: base.session + u64::from(session),
            stream: base.stream + u64::from(stream),
            geometry: base.geometry + u64::from(geometry),
            tracker: base.tracker + u64::from(tracker),
        }
    }

    fn lifecycle_cases() -> Vec<(Lifecycle, State, CropAction, bool)> {
        let base = generations();
        vec![
            (
                Lifecycle::SceneInactive {
                    generations: generations_after(base, false, true, false, false, true),
                },
                State::Manual,
                CropAction::Hold,
                false,
            ),
            (
                Lifecycle::SceneActive {
                    generations: generations_after(base, false, true, false, false, true),
                },
                State::Manual,
                CropAction::Hold,
                false,
            ),
            (
                Lifecycle::CameraDisconnected {
                    generations: generations_after(base, false, false, true, false, true),
                },
                State::Manual,
                CropAction::Hold,
                false,
            ),
            (
                Lifecycle::CameraRestored {
                    generations: generations_after(base, false, false, true, false, true),
                },
                State::Manual,
                CropAction::Hold,
                false,
            ),
            (
                Lifecycle::SourceRebound {
                    generations: generations_after(base, true, true, true, true, true),
                },
                State::Manual,
                CropAction::Wide,
                true,
            ),
            (
                Lifecycle::GeometryChanged {
                    generations: generations_after(base, false, false, true, true, true),
                    source_width: 1920,
                    source_height: 1080,
                },
                State::Manual,
                CropAction::Wide,
                false,
            ),
            (
                Lifecycle::TrackerChanged {
                    generations: generations_after(base, false, false, false, false, true),
                },
                State::Lost,
                CropAction::Hold,
                false,
            ),
            (
                Lifecycle::FramingConfigChanged {
                    generations: generations_after(base, false, true, false, true, true),
                },
                State::Manual,
                CropAction::Wide,
                true,
            ),
            (
                Lifecycle::CandidatePolicyChanged {
                    generations: generations_after(base, false, true, false, false, true),
                },
                State::Manual,
                CropAction::Wide,
                true,
            ),
            (
                Lifecycle::AdapterDestroyed {
                    generations: generations_after(base, false, true, false, false, true),
                },
                State::Manual,
                CropAction::Wide,
                true,
            ),
            (
                Lifecycle::SourceDestroyed {
                    generations: generations_after(base, true, true, true, false, true),
                },
                State::Manual,
                CropAction::Wide,
                true,
            ),
            (
                Lifecycle::ObsShutdown {
                    generations: generations_after(base, true, true, true, false, true),
                },
                State::Manual,
                CropAction::Wide,
                true,
            ),
        ]
    }

    fn lock_test_target(session: &mut TrackingSession) -> Snapshot {
        send(session, 1, Command::EnableAutoFollow);
        observe(session, candidate(10, 100, 1, 2), 2);
        send(
            session,
            3,
            Command::SelectCandidate {
                handle: 10,
                observation_version: 1,
            },
        )
    }

    fn assert_policy_no_op(mut actual: Snapshot, expected: Snapshot) {
        actual.command_result = expected.command_result;
        assert_eq!(actual, expected);
    }

    #[test]
    fn explicit_selection_is_required_for_initial_lock() {
        let mut session = session();
        send(&mut session, 1, Command::EnableAutoFollow);
        observe(&mut session, candidate(10, 100, 1, 2), 2);
        assert_eq!(session.snapshot().state, State::Searching);
        let result = send(
            &mut session,
            3,
            Command::SelectCandidate {
                handle: 10,
                observation_version: 1,
            },
        );
        assert_eq!(result.state, State::Locked);
        assert_eq!(result.crop_action, CropAction::Track);
    }

    #[test]
    fn same_timestamp_face_batch_accepts_older_but_fresh_observations() {
        let mut session = session();
        let later_face = observe(&mut session, candidate(20, 200, 1, 9), 11);
        let earlier_face = observe(&mut session, candidate(21, 201, 1, 8), 11);

        assert_eq!(later_face.observation_version, 1);
        assert_eq!(earlier_face.observation_version, 1);
        assert_eq!(earlier_face.command_result, CommandResult::NotACommand);

        let selected = send(
            &mut session,
            12,
            Command::SelectCandidate {
                handle: 21,
                observation_version: earlier_face.observation_version,
            },
        );
        assert_eq!(selected.state, State::Locked);
        assert_eq!(selected.selected, Some(21));
    }

    #[test]
    fn older_tracker_observation_cannot_refresh_selected_target() {
        let mut session = session();
        let locked = lock_test_target(&mut session);

        let stale = observe(&mut session, candidate(10, 100, 1, 1), 4);
        assert_eq!(
            stale.command_result,
            CommandResult::Rejected(Reason::InvalidCandidate)
        );
        assert_eq!(stale.selected, locked.selected);
        assert_eq!(stale.state, State::Locked);

        let after_deadline = session.apply(Event::Tick { at: 13 });
        assert_eq!(after_deadline.state, State::Searching);
        assert_eq!(after_deadline.reason, Reason::TargetEvidenceStale);
    }

    #[test]
    fn invalid_command_is_a_strict_no_op() {
        let mut session = session();
        let before = session.snapshot();
        let after = send(&mut session, 1, Command::Resume);
        assert_eq!(
            after.command_result,
            CommandResult::Rejected(Reason::InvalidCommand)
        );
        assert_eq!(after.state, before.state);
        assert_eq!(after.crop, before.crop);
        assert_eq!(after.loss_deadline, before.loss_deadline);
    }

    #[test]
    fn stale_selection_is_a_strict_no_op() {
        let mut session = session();
        send(&mut session, 1, Command::EnableAutoFollow);
        observe(&mut session, candidate(10, 100, 1, 2), 2);
        let before = session.snapshot();
        let after = session.apply(Event::Command {
            at: 3,
            envelope: CommandEnvelope {
                identity: before.identity,
                status_version: before.status_version,
                generations: before.generations,
                command: Command::SelectCandidate {
                    handle: 10,
                    observation_version: 99,
                },
            },
        });
        assert_eq!(
            after.command_result,
            CommandResult::Rejected(Reason::StaleCommand)
        );
        assert_eq!(after.state, before.state);
        assert_eq!(after.crop, before.crop);
    }

    #[test]
    fn stale_preview_status_cannot_authorize_a_candidate_selection() {
        let mut session = session();
        send(&mut session, 1, Command::EnableAutoFollow);
        observe(&mut session, candidate(10, 100, 1, 2), 2);
        let preview = session.snapshot();
        session.apply(Event::Tick { at: 3 });
        let before = session.snapshot();

        let after = session.apply(Event::Command {
            at: 4,
            envelope: CommandEnvelope {
                identity: preview.identity,
                status_version: preview.status_version,
                generations: preview.generations,
                command: Command::SelectCandidate {
                    handle: 10,
                    observation_version: preview.observation_version,
                },
            },
        });

        assert_eq!(
            after.command_result,
            CommandResult::Rejected(Reason::StaleCommand)
        );
        assert_policy_no_op(after, before);
    }

    #[test]
    fn unrelated_status_refresh_does_not_drop_valid_freeze_action() {
        let mut session = session();
        let captured = session.snapshot();
        session.apply(Event::Tick { at: 1 });

        let after = session.apply(Event::Command {
            at: 2,
            envelope: CommandEnvelope {
                identity: captured.identity,
                status_version: captured.status_version,
                generations: captured.generations,
                command: Command::Freeze,
            },
        });

        assert_eq!(after.command_result, CommandResult::Applied);
        assert_eq!(after.state, State::Frozen);
        assert_eq!(after.reason, Reason::OperatorFreeze);
    }

    #[test]
    fn delayed_control_action_still_obeys_current_state_validation() {
        let mut session = session();
        let captured = session.snapshot();
        send(&mut session, 1, Command::Freeze);
        let before = session.snapshot();

        let after = session.apply(Event::Command {
            at: 2,
            envelope: CommandEnvelope {
                identity: captured.identity,
                status_version: captured.status_version,
                generations: captured.generations,
                command: Command::EnableAutoFollow,
            },
        });

        assert_eq!(
            after.command_result,
            CommandResult::Rejected(Reason::InvalidCommand)
        );
        assert_policy_no_op(after, before);
    }

    #[test]
    fn non_monotonic_freeze_command_is_a_strict_no_op() {
        let mut session = session();
        lock_test_target(&mut session);
        let before = session.snapshot();

        let after = session.apply(Event::Command {
            at: 2,
            envelope: CommandEnvelope {
                identity: before.identity,
                status_version: before.status_version,
                generations: before.generations,
                command: Command::Freeze,
            },
        });

        assert_eq!(
            after.command_result,
            CommandResult::Rejected(Reason::NonMonotonicEvent)
        );
        assert_policy_no_op(after, before);
    }

    #[test]
    fn continuity_can_recover_only_inside_grace_period() {
        let mut session = session();
        send(&mut session, 1, Command::EnableAutoFollow);
        observe(&mut session, candidate(10, 100, 1, 2), 2);
        send(
            &mut session,
            3,
            Command::SelectCandidate {
                handle: 10,
                observation_version: 1,
            },
        );
        assert_eq!(
            session.apply(Event::Tick { at: 20 }).state,
            State::Searching
        );
        assert_eq!(
            observe(&mut session, candidate(11, 100, 2, 30), 30).state,
            State::Locked
        );
    }

    #[test]
    fn continuity_grace_starts_when_evidence_becomes_stale() {
        let mut session = session();
        lock_test_target(&mut session);

        // Evidence at 2 ms becomes stale at 13 ms. The full 100 ms recovery
        // interval runs through 112 ms, expiring at 113 ms. Evidence age and
        // loss grace are separate windows; subtracting last_seen from the
        // candidate time would incorrectly shorten this grace period.
        assert_eq!(
            session.apply(Event::Tick { at: 20 }).state,
            State::Searching
        );
        let recovered = observe(&mut session, candidate(11, 100, 2, 112), 112);

        assert_eq!(recovered.state, State::Locked);
        assert_eq!(recovered.reason, Reason::ContinuityRecovered);
        assert_eq!(recovered.selected, Some(11));
        assert_eq!(recovered.loss_deadline, None);
    }

    #[test]
    fn delayed_same_identity_candidate_cannot_reacquire_after_deadline() {
        let mut session = session();
        lock_test_target(&mut session);
        let before = session.snapshot();

        // Last evidence was at 2 ms, so the lock becomes stale at 13 ms and
        // its 100 ms grace period expires at 113 ms. No periodic Tick is
        // queued between the detector result and that deadline.
        let late = candidate(11, 100, 2, 113);
        let after = observe(&mut session, late, 113);

        assert_eq!(after.state, State::Lost);
        assert_eq!(after.reason, Reason::TargetLossDeadlineExpired);
        assert_eq!(after.selected, None);
        assert_eq!(after.crop, before.crop);
        assert_eq!(after.crop_action, CropAction::Hold);
    }

    #[test]
    fn evidence_arriving_after_lock_staleness_enters_searching_before_recovery() {
        let mut session = session();
        lock_test_target(&mut session);

        // The result is fresh, but a prior periodic Tick was delayed. The
        // executor must still recognize the stale interval before applying
        // the result. Matching continuity may recover inside the grace.
        let recovered = observe(&mut session, candidate(11, 100, 2, 14), 14);

        assert_eq!(recovered.state, State::Locked);
        assert_eq!(recovered.reason, Reason::ContinuityRecovered);
        assert_eq!(recovered.selected, Some(11));
        assert_eq!(recovered.crop_action, CropAction::Track);
        assert_eq!(recovered.status_version, 5);
    }

    #[test]
    fn deadline_transition_is_independent_of_rejected_command() {
        let mut session = session();
        lock_test_target(&mut session);
        let stale_preview = session.snapshot();

        let after = session.apply(Event::Command {
            at: 113,
            envelope: CommandEnvelope {
                identity: stale_preview.identity,
                status_version: stale_preview.status_version,
                generations: stale_preview.generations,
                command: Command::SelectCandidate {
                    handle: 10,
                    observation_version: stale_preview.observation_version,
                },
            },
        });

        assert_eq!(
            after.command_result,
            CommandResult::Rejected(Reason::StaleCommand)
        );
        assert_eq!(after.state, State::Lost);
        assert_eq!(after.reason, Reason::TargetLossDeadlineExpired);
        assert_eq!(after.selected, None);
        assert_eq!(after.crop, stale_preview.crop);
        assert_eq!(after.crop_action, CropAction::Hold);
        assert_eq!(after.loss_deadline, None);
        assert_eq!(after.status_version, stale_preview.status_version + 2);
    }

    #[test]
    fn malformed_command_rejection_keeps_due_loss_deadline_independent() {
        let mut session = session();
        lock_test_target(&mut session);
        let before = session.snapshot();

        let after = session.reject_invalid_command_at(
            113,
            before.identity,
            before.generations,
            before.status_version,
        );

        assert_eq!(
            after.command_result,
            CommandResult::Rejected(Reason::StaleCommand)
        );
        assert_eq!(after.state, State::Lost);
        assert_eq!(after.reason, Reason::TargetLossDeadlineExpired);
        assert_eq!(after.selected, None);
        assert_eq!(after.generations, before.generations);
        assert_eq!(after.crop, before.crop);
        assert_eq!(after.crop_action, CropAction::Hold);
        assert_eq!(after.status_version, before.status_version + 2);
    }

    #[test]
    fn ambiguous_token_does_not_recover_lock() {
        let mut session = session();
        send(&mut session, 1, Command::EnableAutoFollow);
        observe(&mut session, candidate(10, 100, 1, 2), 2);
        send(
            &mut session,
            3,
            Command::SelectCandidate {
                handle: 10,
                observation_version: 1,
            },
        );
        session.apply(Event::Tick { at: 20 });
        observe(&mut session, candidate(11, 200, 2, 30), 30);
        assert_eq!(session.snapshot().state, State::Searching);
    }

    #[test]
    fn reused_candidate_handle_with_different_continuity_is_rejected() {
        let mut session = session();
        lock_test_target(&mut session);
        let before = session.snapshot();
        let recycled = candidate(10, 200, before.observation_version + 1, 4);

        let after = observe(&mut session, recycled, 4);

        assert_eq!(
            after.command_result,
            CommandResult::Rejected(Reason::InvalidCandidate)
        );
        assert_policy_no_op(after, before);
    }

    #[test]
    fn evicted_candidate_handles_cannot_be_recycled() {
        let mut session = TrackingSession::new(
            Config {
                loss_grace_ms: 100,
                max_evidence_age_ms: 10,
                max_candidates: 1,
                ..Config::default()
            },
            identity(),
            generations(),
        )
        .unwrap();
        observe(&mut session, candidate(10, 100, 1, 2), 2);
        observe(&mut session, candidate(11, 110, 2, 3), 3);
        assert_eq!(session.latest.len(), 1);
        assert!(!session.latest.contains_key(&10));
        let before = session.snapshot();

        let after = observe(&mut session, candidate(10, 200, 3, 4), 4);

        assert_eq!(
            after.command_result,
            CommandResult::Rejected(Reason::InvalidCandidate)
        );
        assert_policy_no_op(after, before);
        assert_eq!(session.highest_candidate_handle, 11);
    }

    #[test]
    fn reset_wide_stops_following_and_requires_explicit_rearm() {
        let mut session = session();
        send(&mut session, 1, Command::EnableAutoFollow);
        let searching = session.snapshot();
        assert_eq!(searching.state, State::Searching);
        let lost = session.apply(Event::Tick { at: 101 });
        assert_eq!(lost.state, State::Lost);
        let reset = send(&mut session, 102, Command::ResetToWide);
        assert_eq!(reset.state, State::Manual);
        assert_eq!(reset.selected, None);
        assert_eq!(reset.crop_action, CropAction::Wide);
        assert_eq!(
            send(&mut session, 103, Command::EnableAutoFollow).state,
            State::Searching
        );
    }

    #[test]
    fn reset_wide_from_locked_clears_target_and_full_frames() {
        let mut engine = TrackingEngine::new(
            Config::default(),
            framing::FramingConfig::default(),
            identity(),
            generations(),
        )
        .unwrap();
        let initial = engine.snapshot();
        engine.apply(Event::Command {
            at: 1,
            envelope: CommandEnvelope {
                identity: initial.identity,
                status_version: initial.status_version,
                generations: initial.generations,
                command: Command::EnableAutoFollow,
            },
        });
        engine.apply(Event::Candidate {
            at: 2,
            candidate: candidate(10, 100, 1, 2),
        });
        let searching = engine.snapshot();
        let locked = engine.apply(Event::Command {
            at: 3,
            envelope: CommandEnvelope {
                identity: searching.identity,
                status_version: searching.status_version,
                generations: searching.generations,
                command: Command::SelectCandidate {
                    handle: 10,
                    observation_version: searching.observation_version,
                },
            },
        });
        assert_eq!(locked.state, State::Locked);
        assert_ne!(locked.crop, Rect::full_frame());

        let wide = engine.apply(Event::Command {
            at: 4,
            envelope: CommandEnvelope {
                identity: locked.identity,
                status_version: locked.status_version,
                generations: locked.generations,
                command: Command::ResetToWide,
            },
        });

        assert_eq!(wide.command_result, CommandResult::Applied);
        assert_eq!(wide.state, State::Manual);
        assert_eq!(wide.selected, None);
        assert_eq!(wide.crop_action, CropAction::Wide);
        assert_eq!(wide.crop, Rect::full_frame());
    }

    #[test]
    fn reset_wide_preserves_an_explicit_frozen_state() {
        let mut session = session();
        lock_test_target(&mut session);
        send(&mut session, 4, Command::Freeze);
        let frozen = session.snapshot();

        let wide = send(&mut session, 5, Command::ResetToWide);

        assert_eq!(wide.command_result, CommandResult::Applied);
        assert_eq!(wide.state, State::Frozen);
        assert_eq!(wide.selected, None);
        assert_eq!(wide.crop, Rect::full_frame());
        assert_eq!(wide.crop_action, CropAction::Wide);
        assert_eq!(frozen.state, State::Frozen);
    }

    #[test]
    fn lifecycle_invalidates_selection_and_requires_manual_recovery() {
        let mut session = session();
        send(&mut session, 1, Command::EnableAutoFollow);
        observe(&mut session, candidate(10, 100, 1, 2), 2);
        send(
            &mut session,
            3,
            Command::SelectCandidate {
                handle: 10,
                observation_version: 1,
            },
        );
        let mut changed = generations();
        changed.stream += 1;
        changed.geometry += 1;
        changed.tracker += 1;
        let snapshot = session.apply(Event::Lifecycle {
            at: 4,
            identity: identity(),
            event: Lifecycle::GeometryChanged {
                generations: changed,
                source_width: 1920,
                source_height: 1080,
            },
        });
        assert_eq!(snapshot.state, State::Manual);
        assert_eq!(snapshot.selected, None);
        assert_eq!(snapshot.generations, changed);
        assert_eq!(snapshot.crop_action, CropAction::Wide);
    }

    #[test]
    fn lifecycle_generation_matrix_and_recovery_are_exact() {
        for (index, (event, expected_state, expected_crop_action, closes_session)) in
            lifecycle_cases().into_iter().enumerate()
        {
            let mut session = session();
            lock_test_target(&mut session);
            let before = session.snapshot();
            let after = session.apply(Event::Lifecycle {
                at: 4,
                identity: before.identity,
                event,
            });

            assert_eq!(
                after.command_result,
                CommandResult::NotACommand,
                "case {index}"
            );
            assert_eq!(after.state, expected_state, "case {index}");
            assert_eq!(
                after.reason,
                if closes_session {
                    Reason::SessionClosing
                } else if expected_state == State::Lost {
                    Reason::TrackerIdentityExpired
                } else {
                    Reason::LifecycleInvalidated
                },
                "case {index}"
            );
            assert_eq!(after.selected, None, "case {index}");
            assert_eq!(after.loss_deadline, None, "case {index}");
            assert_eq!(after.crop_action, expected_crop_action, "case {index}");
            assert_eq!(
                after.crop,
                if expected_crop_action == CropAction::Wide {
                    Config::default().safe_wide
                } else {
                    before.crop
                },
                "case {index}"
            );
            assert_eq!(
                after.generations,
                match event {
                    Lifecycle::SceneInactive { generations }
                    | Lifecycle::SceneActive { generations }
                    | Lifecycle::CameraDisconnected { generations }
                    | Lifecycle::CameraRestored { generations }
                    | Lifecycle::SourceRebound { generations }
                    | Lifecycle::GeometryChanged { generations, .. }
                    | Lifecycle::TrackerChanged { generations }
                    | Lifecycle::FramingConfigChanged { generations }
                    | Lifecycle::CandidatePolicyChanged { generations }
                    | Lifecycle::AdapterDestroyed { generations }
                    | Lifecycle::SourceDestroyed { generations }
                    | Lifecycle::ObsShutdown { generations } => generations,
                },
                "case {index}"
            );
            assert_eq!(
                after.status_version,
                before.status_version + 1,
                "case {index}"
            );
            assert_eq!(
                after.observation_version,
                before.observation_version + 1,
                "case {index}"
            );
            assert_eq!(after.closed, closes_session, "case {index}");

            let mut stale_candidate = candidate(11, 100, 2, 5);
            stale_candidate.generations = before.generations;
            let rejected_candidate = observe(&mut session, stale_candidate, 5);
            assert!(matches!(
                rejected_candidate.command_result,
                CommandResult::Rejected(_)
            ));
            assert_policy_no_op(rejected_candidate, after);

            let stale_command = session.apply(Event::Command {
                at: 6,
                envelope: CommandEnvelope {
                    identity: before.identity,
                    status_version: before.status_version,
                    generations: before.generations,
                    command: Command::EnableAutoFollow,
                },
            });
            assert_eq!(
                stale_command.command_result,
                CommandResult::Rejected(if closes_session {
                    Reason::SessionClosed
                } else {
                    Reason::StaleCommand
                }),
                "case {index}"
            );
            assert_policy_no_op(stale_command, after);
        }
    }

    #[test]
    fn rapid_scene_inactive_active_cycle_requires_fresh_selection() {
        let mut session = session();
        lock_test_target(&mut session);
        let locked = session.snapshot();
        let inactive_generations =
            generations_after(locked.generations, false, true, false, false, true);
        let inactive = session.apply(Event::Lifecycle {
            at: 4,
            identity: locked.identity,
            event: Lifecycle::SceneInactive {
                generations: inactive_generations,
            },
        });
        let active_generations =
            generations_after(inactive.generations, false, true, false, false, true);
        let active = session.apply(Event::Lifecycle {
            at: 5,
            identity: locked.identity,
            event: Lifecycle::SceneActive {
                generations: active_generations,
            },
        });

        assert_eq!(active.state, State::Manual);
        assert_eq!(active.selected, None);
        assert_eq!(active.crop_action, CropAction::Hold);
        assert_eq!(active.crop, locked.crop);
        assert_eq!(active.generations.session, locked.generations.session + 2);
        assert_eq!(active.generations.tracker, locked.generations.tracker + 2);

        let mut late = candidate(11, 100, active.observation_version, 6);
        late.generations = locked.generations;
        let rejected_late_result = observe(&mut session, late, 6);
        assert_eq!(
            rejected_late_result.command_result,
            CommandResult::Rejected(Reason::InvalidCandidate)
        );
        assert_policy_no_op(rejected_late_result, active);

        let rearmed = send(&mut session, 7, Command::EnableAutoFollow);
        assert_eq!(rearmed.state, State::Searching);
        assert_eq!(rearmed.selected, None);
    }

    #[test]
    fn mismatched_and_inexact_lifecycle_events_are_strict_no_ops() {
        let mut session = session();
        lock_test_target(&mut session);
        let before = session.snapshot();
        let expected = generations_after(before.generations, false, true, false, false, true);

        let mismatched_identity = session.apply(Event::Lifecycle {
            at: 4,
            identity: Identity {
                source: before.identity.source + 1,
                session: before.identity.session,
            },
            event: Lifecycle::SceneInactive {
                generations: expected,
            },
        });
        assert_eq!(
            mismatched_identity.command_result,
            CommandResult::Rejected(Reason::StaleLifecycle)
        );
        assert_policy_no_op(mismatched_identity, before);

        let inexact_generations = session.apply(Event::Lifecycle {
            at: 4,
            identity: before.identity,
            event: Lifecycle::SceneInactive {
                generations: before.generations,
            },
        });
        assert_eq!(
            inexact_generations.command_result,
            CommandResult::Rejected(Reason::InvalidLifecycle)
        );
        assert_policy_no_op(inexact_generations, before);
    }

    #[test]
    fn invalid_geometry_is_rejected() {
        assert_eq!(
            Rect::normalized(f32::NAN, 0.0, 1.0, 1.0),
            Err(GeometryError::NonFinite)
        );
        assert_eq!(
            Rect::normalized(0.9, 0.0, 0.2, 1.0),
            Err(GeometryError::OutOfBounds)
        );
    }

    #[test]
    fn zero_candidate_identifiers_are_rejected_as_strict_no_ops() {
        for invalid in 0..3 {
            let mut session = session();
            send(&mut session, 1, Command::EnableAutoFollow);
            let before = session.snapshot();
            let mut candidate = candidate(10, 100, 1, 2);
            match invalid {
                0 => candidate.handle = 0,
                1 => candidate.continuity = 0,
                _ => candidate.observation_version = 0,
            }

            let after = observe(&mut session, candidate, 2);

            assert_eq!(
                after.command_result,
                CommandResult::Rejected(Reason::InvalidCandidate),
                "invalid identifier case {invalid}"
            );
            assert_policy_no_op(after, before);
        }
    }

    #[test]
    fn lower_version_candidate_cannot_move_a_locked_crop() {
        let mut session = session();
        send(&mut session, 1, Command::EnableAutoFollow);
        observe(&mut session, candidate(10, 100, 2, 2), 2);
        send(
            &mut session,
            3,
            Command::SelectCandidate {
                handle: 10,
                observation_version: 2,
            },
        );
        let before = session.snapshot();
        let stale = observe(&mut session, candidate(11, 100, 1, 4), 4);
        assert_eq!(
            stale.command_result,
            CommandResult::Rejected(Reason::InvalidCandidate)
        );
        assert_eq!(stale.state, before.state);
        assert_eq!(stale.crop, before.crop);
    }

    #[test]
    fn expired_candidate_result_is_a_strict_no_op() {
        let mut session = session();
        send(&mut session, 1, Command::EnableAutoFollow);
        let before = session.snapshot();
        let mut expired = candidate(10, 100, 1, 2);
        expired.expires_at = 5;

        let after = observe(&mut session, expired, 6);

        assert_eq!(
            after.command_result,
            CommandResult::Rejected(Reason::InvalidCandidate)
        );
        assert_eq!(after.state, before.state);
        assert_eq!(after.selected, before.selected);
        assert_eq!(after.crop, before.crop);
        assert_eq!(after.loss_deadline, before.loss_deadline);
        assert_eq!(after.status_version, before.status_version);
        assert_eq!(after.observation_version, before.observation_version);
    }

    #[test]
    fn cross_session_commands_and_candidates_are_rejected() {
        let first = session();
        let mut second = TrackingSession::new(
            Config::default(),
            Identity {
                source: 102,
                session: 202,
            },
            generations(),
        )
        .unwrap();
        let before = second.snapshot();
        let after_candidate = observe(&mut second, candidate(10, 100, 1, 0), 0);
        assert_eq!(
            after_candidate.command_result,
            CommandResult::Rejected(Reason::InvalidCandidate)
        );
        assert_eq!(after_candidate.state, before.state);
        assert_eq!(after_candidate.status_version, before.status_version);

        let first_snapshot = first.snapshot();
        let command = Event::Command {
            at: 1,
            envelope: CommandEnvelope {
                identity: first_snapshot.identity,
                status_version: first_snapshot.status_version,
                generations: first_snapshot.generations,
                command: Command::EnableAutoFollow,
            },
        };
        let rejected = second.apply(command);
        assert_eq!(
            rejected.command_result,
            CommandResult::Rejected(Reason::StaleCommand)
        );
        assert_eq!(rejected.state, before.state);
        assert_eq!(rejected.crop, before.crop);
        assert_eq!(rejected.loss_deadline, before.loss_deadline);
        assert_eq!(rejected.status_version, before.status_version);
    }

    #[test]
    fn zero_identity_cannot_create_a_session() {
        assert!(matches!(
            TrackingSession::new(
                Config::default(),
                Identity {
                    source: 0,
                    session: 1,
                },
                generations(),
            ),
            Err(SessionConfigError::InvalidIdentity)
        ));
    }

    #[test]
    fn engine_converts_selected_face_to_bounded_crop() {
        let mut engine = TrackingEngine::new(
            Config::default(),
            framing::FramingConfig::default(),
            identity(),
            generations(),
        )
        .unwrap();
        let initial = engine.snapshot();
        engine.apply(Event::Command {
            at: 1,
            envelope: CommandEnvelope {
                identity: initial.identity,
                status_version: initial.status_version,
                generations: initial.generations,
                command: Command::EnableAutoFollow,
            },
        });
        engine.apply(Event::Candidate {
            at: 2,
            candidate: candidate(10, 100, 1, 2),
        });
        let searching = engine.snapshot();
        let target = candidate(10, 100, 1, 2).rect;
        let locked = engine.apply(Event::Command {
            at: 3,
            envelope: CommandEnvelope {
                identity: searching.identity,
                status_version: searching.status_version,
                generations: searching.generations,
                command: Command::SelectCandidate {
                    handle: 10,
                    observation_version: searching.observation_version,
                },
            },
        });
        assert_eq!(locked.state, State::Locked);
        assert_eq!(locked.selected_rect, Some(target));
        assert_ne!(locked.crop, target);
        assert!((locked.crop.width / locked.crop.height - 1.0).abs() < 0.000_1);
        assert!(locked.crop.x + locked.crop.width <= 1.0);
        assert!(locked.crop.y + locked.crop.height <= 1.0);
        assert_eq!(locked.viewport, Rect::full_frame());
    }

    #[test]
    fn rejected_command_preserves_framing_plan() {
        let mut engine = TrackingEngine::new(
            Config::default(),
            framing::FramingConfig::default(),
            identity(),
            generations(),
        )
        .unwrap();
        let initial = engine.snapshot();
        engine.apply(Event::Command {
            at: 1,
            envelope: CommandEnvelope {
                identity: initial.identity,
                status_version: initial.status_version,
                generations: initial.generations,
                command: Command::EnableAutoFollow,
            },
        });
        engine.apply(Event::Candidate {
            at: 2,
            candidate: candidate(10, 100, 1, 2),
        });
        let searching = engine.snapshot();
        let locked = engine.apply(Event::Command {
            at: 3,
            envelope: CommandEnvelope {
                identity: searching.identity,
                status_version: searching.status_version,
                generations: searching.generations,
                command: Command::SelectCandidate {
                    handle: 10,
                    observation_version: searching.observation_version,
                },
            },
        });
        let mut stale_generations = locked.generations;
        stale_generations.session += 1;
        let rejected = engine.apply(Event::Command {
            at: 4,
            envelope: CommandEnvelope {
                identity: locked.identity,
                status_version: locked.status_version - 1,
                generations: stale_generations,
                command: Command::ManualOverride,
            },
        });
        assert_eq!(
            rejected.command_result,
            CommandResult::Rejected(Reason::StaleCommand)
        );
        assert_eq!(rejected.state, locked.state);
        assert_eq!(rejected.selected, locked.selected);
        assert_eq!(rejected.selected_rect, locked.selected_rect);
        assert_eq!(rejected.crop, locked.crop);
        assert_eq!(rejected.viewport, locked.viewport);
        assert_eq!(rejected.loss_deadline, locked.loss_deadline);
        assert_eq!(rejected.status_version, locked.status_version);
    }

    #[test]
    fn geometry_change_recomputes_safe_wide_padding_before_publish() {
        let mut engine = TrackingEngine::new(
            Config::default(),
            framing::FramingConfig::default(),
            identity(),
            generations(),
        )
        .unwrap();
        let before = engine.snapshot();
        let changed = generations_after(before.generations, false, false, true, true, true);
        let after = engine.apply(Event::Lifecycle {
            at: 1,
            identity: before.identity,
            event: Lifecycle::GeometryChanged {
                generations: changed,
                source_width: 1280,
                source_height: 960,
            },
        });
        assert_eq!(after.state, State::Manual);
        assert_eq!(after.selected, None);
        assert_eq!(after.crop, Rect::full_frame());
        assert!((after.viewport.width - 0.75).abs() < 0.000_1);
        assert_eq!(after.viewport.x, 0.125);
    }

    #[test]
    fn invalid_geometry_change_is_a_strict_no_op() {
        let mut engine = TrackingEngine::new(
            Config::default(),
            framing::FramingConfig::default(),
            identity(),
            generations(),
        )
        .unwrap();
        let before = engine.snapshot();
        let after = engine.apply(Event::Lifecycle {
            at: 1,
            identity: before.identity,
            event: Lifecycle::GeometryChanged {
                generations: generations_after(before.generations, false, false, true, true, true),
                source_width: 0,
                source_height: 0,
            },
        });
        assert_eq!(
            after.command_result,
            CommandResult::Rejected(Reason::InvalidLifecycle)
        );
        assert_eq!(after.state, before.state);
        assert_eq!(after.generations, before.generations);
        assert_eq!(after.crop, before.crop);
        assert_eq!(after.viewport, before.viewport);
        assert_eq!(after.status_version, before.status_version);
    }

    #[test]
    fn invalid_geometry_event_does_not_suppress_an_independent_loss_deadline() {
        let mut engine = TrackingEngine::new(
            Config::default(),
            framing::FramingConfig::default(),
            identity(),
            generations(),
        )
        .unwrap();
        let initial = engine.snapshot();
        engine.apply(Event::Command {
            at: 1,
            envelope: CommandEnvelope {
                identity: initial.identity,
                status_version: initial.status_version,
                generations: initial.generations,
                command: Command::EnableAutoFollow,
            },
        });
        engine.apply(Event::Candidate {
            at: 2,
            candidate: candidate(10, 100, 1, 2),
        });
        let searching = engine.snapshot();
        let locked = engine.apply(Event::Command {
            at: 3,
            envelope: CommandEnvelope {
                identity: searching.identity,
                status_version: searching.status_version,
                generations: searching.generations,
                command: Command::SelectCandidate {
                    handle: 10,
                    observation_version: searching.observation_version,
                },
            },
        });
        let generations_before = locked.generations;
        let rejected = engine.apply(Event::Lifecycle {
            at: 5_503,
            identity: locked.identity,
            event: Lifecycle::GeometryChanged {
                generations: generations_after(generations_before, false, false, true, true, true),
                source_width: 0,
                source_height: 0,
            },
        });

        assert_eq!(
            rejected.command_result,
            CommandResult::Rejected(Reason::InvalidLifecycle)
        );
        assert_eq!(rejected.state, State::Lost);
        assert_eq!(rejected.reason, Reason::TargetLossDeadlineExpired);
        assert_eq!(rejected.selected, None);
        assert_eq!(rejected.generations, generations_before);
        assert_eq!(rejected.crop, locked.crop);
        assert_eq!(rejected.crop_action, CropAction::Hold);
        assert!(rejected.status_version > locked.status_version);
    }
}
