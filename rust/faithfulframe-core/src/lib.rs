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
    pub status_version: u64,
    pub generations: Generations,
    pub command: Command,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Lifecycle {
    SceneInactive,
    SceneActive,
    CameraDisconnected,
    CameraRestored,
    SourceRebound { generations: Generations },
    GeometryChanged { generations: Generations },
    TrackerChanged { generations: Generations },
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Candidate {
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
    pub crop_action: CropAction,
    pub crop: Rect,
    pub loss_deadline: Option<Millis>,
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
    handles: BTreeMap<CandidateHandle, ContinuityToken>,
    held_crop: Rect,
    crop_action: CropAction,
    loss_deadline: Option<Millis>,
    status_version: u64,
    observation_version: u64,
    last_at: Millis,
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
            handles: BTreeMap::new(),
            held_crop: safe_wide,
            crop_action: CropAction::None,
            loss_deadline: None,
            status_version: 0,
            observation_version: 0,
            last_at: 0,
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
            crop_action: self.crop_action,
            crop: self.held_crop,
            loss_deadline: self.loss_deadline,
            command_result: CommandResult::NotACommand,
        }
    }

    pub fn apply(&mut self, event: Event) -> Snapshot {
        let at = event_time(event);
        if at < self.last_at {
            return self.reject(Reason::NonMonotonicEvent);
        }
        let result = match event {
            Event::Command { envelope, .. } => {
                if envelope.identity != self.identity
                    || envelope.status_version != self.status_version
                    || envelope.generations != self.generations
                {
                    self.reject_command(Reason::StaleCommand)
                } else {
                    self.apply_command(envelope.command, at)
                }
            }
            Event::Candidate { at, candidate } => self.apply_candidate(candidate, at),
            Event::Tick { .. } => self.apply_tick(at),
            Event::Lifecycle { event, .. } => self.apply_lifecycle(event),
        };
        if !matches!(result, CommandResult::Rejected(_)) {
            self.last_at = at;
            self.status_version = self.status_version.saturating_add(1);
        }
        let mut snapshot = self.snapshot();
        snapshot.command_result = result;
        snapshot
    }

    fn apply_command(&mut self, command: Command, at: Millis) -> CommandResult {
        match command {
            Command::Freeze if self.state != State::Frozen => {
                self.state = State::Frozen;
                self.reason = Reason::OperatorFreeze;
                self.crop_action = CropAction::Hold;
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
                self.loss_deadline = None;
                self.crop_action = CropAction::None;
                CommandResult::Applied
            }
            Command::EnableAutoFollow if matches!(self.state, State::Manual | State::Lost) => {
                self.state = State::Searching;
                self.reason = Reason::AwaitingSelection;
                self.selected = None;
                self.loss_deadline = Some(at.saturating_add(self.config.loss_grace_ms));
                self.crop_action = CropAction::Hold;
                CommandResult::Applied
            }
            Command::ResetToWide if matches!(self.state, State::Searching | State::Lost) => {
                self.held_crop = self.config.safe_wide;
                self.crop_action = CropAction::Wide;
                self.reason = Reason::ResetWide;
                CommandResult::Applied
            }
            Command::SelectCandidate {
                handle,
                observation_version,
            } if matches!(self.state, State::Manual | State::Searching | State::Lost)
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
                    });
                    self.state = State::Locked;
                    self.reason = Reason::SelectedByOperator;
                    self.held_crop = candidate.rect;
                    self.crop_action = CropAction::Track;
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
        if candidate.identity != self.identity
            || candidate.generations != self.generations
            || candidate.observed_at < self.last_at
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
        if let Some(previous) = self.handles.get(&candidate.handle)
            && *previous != candidate.continuity
        {
            return self.reject_command(Reason::InvalidCandidate);
        }
        self.handles.insert(candidate.handle, candidate.continuity);
        self.latest.insert(candidate.handle, candidate);
        while self.latest.len() > self.config.max_candidates {
            if let Some(oldest) = self.latest.keys().next().copied() {
                self.latest.remove(&oldest);
            }
        }
        self.observation_version = self.observation_version.max(candidate.observation_version);

        if let Some(selected) = self.selected {
            if selected.continuity == candidate.continuity && self.state == State::Locked {
                self.selected = Some(SelectedTarget {
                    handle: candidate.handle,
                    last_seen: candidate.observed_at,
                    ..selected
                });
                self.held_crop = candidate.rect;
                self.crop_action = CropAction::Track;
            } else if selected.continuity == candidate.continuity
                && self.state == State::Searching
                && self
                    .loss_deadline
                    .is_some_and(|deadline| candidate.observed_at <= deadline)
                && candidate.observed_at.saturating_sub(selected.last_seen)
                    <= self.config.loss_grace_ms
            {
                self.selected = Some(SelectedTarget {
                    handle: candidate.handle,
                    last_seen: candidate.observed_at,
                    ..selected
                });
                self.state = State::Locked;
                self.reason = Reason::ContinuityRecovered;
                self.held_crop = candidate.rect;
                self.crop_action = CropAction::Track;
                self.loss_deadline = None;
            }
        }
        CommandResult::NotACommand
    }

    fn apply_tick(&mut self, at: Millis) -> CommandResult {
        if self.state == State::Locked
            && self.selected.is_some_and(|target| {
                at.saturating_sub(target.last_seen) > self.config.max_evidence_age_ms
            })
        {
            self.state = State::Searching;
            self.reason = Reason::TargetEvidenceStale;
            self.crop_action = CropAction::Hold;
            self.loss_deadline = Some(at.saturating_add(self.config.loss_grace_ms));
        } else if self.state == State::Searching
            && self.loss_deadline.is_some_and(|deadline| at >= deadline)
        {
            self.state = State::Lost;
            self.reason = Reason::TargetLossDeadlineExpired;
            self.crop_action = CropAction::Hold;
            self.loss_deadline = None;
        }
        CommandResult::NotACommand
    }

    fn apply_lifecycle(&mut self, event: Lifecycle) -> CommandResult {
        match event {
            Lifecycle::SceneInactive | Lifecycle::SceneActive => {
                self.invalidate_to_manual(self.generations, Reason::LifecycleInvalidated);
            }
            Lifecycle::CameraDisconnected | Lifecycle::CameraRestored => {
                self.invalidate_to_manual(self.generations, Reason::LifecycleInvalidated);
            }
            Lifecycle::SourceRebound { generations }
            | Lifecycle::GeometryChanged { generations }
            | Lifecycle::TrackerChanged { generations } => {
                self.invalidate_to_manual(generations, Reason::LifecycleInvalidated);
            }
        }
        CommandResult::NotACommand
    }

    fn invalidate_to_manual(&mut self, generations: Generations, reason: Reason) {
        self.generations = generations;
        self.selected = None;
        self.latest.clear();
        self.loss_deadline = None;
        self.state = State::Manual;
        self.reason = reason;
        self.held_crop = self.config.safe_wide;
        self.crop_action = CropAction::Wide;
    }

    fn reject_command(&self, reason: Reason) -> CommandResult {
        CommandResult::Rejected(reason)
    }

    fn reject(&self, reason: Reason) -> Snapshot {
        let mut snapshot = self.snapshot();
        snapshot.command_result = CommandResult::Rejected(reason);
        snapshot
    }
}

fn event_time(event: Event) -> Millis {
    match event {
        Event::Command { at, .. } | Event::Tick { at } | Event::Lifecycle { at, .. } => at,
        Event::Candidate { at, .. } => at,
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
    fn invalid_command_is_a_strict_no_op() {
        let mut session = session();
        let before = session.snapshot();
        let after = send(&mut session, 1, Command::ResetToWide);
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
    fn loss_holds_and_reset_wide_stays_lost() {
        let mut session = session();
        send(&mut session, 1, Command::EnableAutoFollow);
        let searching = session.snapshot();
        assert_eq!(searching.state, State::Searching);
        let lost = session.apply(Event::Tick { at: 101 });
        assert_eq!(lost.state, State::Lost);
        let reset = send(&mut session, 102, Command::ResetToWide);
        assert_eq!(reset.state, State::Lost);
        assert_eq!(reset.crop_action, CropAction::Wide);
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
        changed.geometry += 1;
        let snapshot = session.apply(Event::Lifecycle {
            at: 4,
            event: Lifecycle::GeometryChanged {
                generations: changed,
            },
        });
        assert_eq!(snapshot.state, State::Manual);
        assert_eq!(snapshot.selected, None);
        assert_eq!(snapshot.generations, changed);
        assert_eq!(snapshot.crop_action, CropAction::Wide);
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
}
