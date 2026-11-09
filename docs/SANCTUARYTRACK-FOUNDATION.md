# FaithfulFrame foundation audit and delivery plan

## Baseline audited

- Upstream: `norihiro/obs-face-tracker`, commit `046448b` (clean worktree).
- License: GPL-2.0 (`LICENSE`). FaithfulFrame remains GPL-compatible and must
  retain upstream notices and bundled dlib/OpenBLAS/model license notices.
- Submodules: dlib `6d338db` and libvisca `03578b0` are required and now
  initialized locally.
- Baseline result on this host: **not built**. This arm64 Mac has Command Line
  Tools selected but no Xcode compiler, Ninja, or OBS development environment.
  CMake therefore stopped before configuration. This is a dependency blocker,
  not a source failure.
- Windows result: **not verified**. The local GitHub CLI credentials are expired,
  so the existing upstream Actions run cannot yet be inspected or triggered.
  A successful Windows compile will be recorded separately from runtime testing.

## Technical audit

| Area | Existing implementation | FaithfulFrame disposition |
| --- | --- | --- |
| OBS integration | Source, effect filter, PTZ filter, monitor source; source-specific settings | Keep source/filter isolation; make the designated source explicit |
| Detection | dlib HOG and CNN, periodic staged texture readback | Keep dlib first; benchmark before replacing it |
| Tracking | dlib correlation trackers with confidence attenuation | Add a target policy/identity continuity layer above detections |
| Crop | Existing PID-like pan/zoom controls, deadbands, aspect/scale limits | Extract normalized crop geometry and bounded motion controller |
| Dock | Qt dock with OBS display preview and target selection | Replace its operator surface incrementally; retain OBS native integration |
| Build | CMake, static dlib, platform installers, macOS/Linux CI | Modernize CI matrix and add Windows packaging/verification receipts |
| Tests | Plugin-load test via onsdriver | Add offline deterministic engine and video scenarios |

Important constraints found upstream: the README acknowledges CPU growth and
frame vibration; `face_tracker_manager` currently exposes mutable tracking
state and supports multiple transient trackers; `face-tracker.cpp` mixes OBS
integration, settings, sampling, and crop control. These are the main reasons
to add seams rather than rewrite all tracking at once.

## UI architecture decision record (ADR-001)

**Decision:** ship native OBS properties for setup and a native Qt dock for
live operation. Do not ship React/Astryx web UI in the initial product.

**Why:** properties are portable, serialized with OBS collections, accessible
to existing OBS workflows, and already supported upstream. Qt provides a dock,
keyboard control, palette integration, and an efficient OBS preview without a
second capture pipeline. A browser panel adds packaging, accessibility, and
runtime failure surface without solving a live-service need.

**Consequences:** `DESIGN.md` adapts Astryx principles rather than components.
The dock must stay thin, consume snapshots, and issue commands through the
engine. A web panel may be reconsidered only after native usability testing
shows a requirement Qt cannot meet.

## Initial UI specification

```
FaithfulFrame                                    [source: Sanctuary Camera]
● LOCKED  Following speaker                         Zoom 1.8×
┌──────────────── compact OBS preview ─────────────────────────────┐
│  stage boundary + current crop / target overlay (optional)        │
└───────────────────────────────────────────────────────────────────┘
[ Freeze tracking ]              primary action changes by state
[ Reset to Wide ] [ Manual Override ]
Preset: [ Sunday speaker ▾ ]                         [ Camera Setup ]
```

State behavior: LOCKED shows **Freeze tracking**; SEARCHING explains the grace
timer; LOST shows **Reset to Wide** as primary; FROZEN and MANUAL show **Auto
Follow**. A disabled action states its condition. Setup places region and crop
preview together, then framing/zoom, then save/restore. Advanced is collapsed.

## File-level implementation plan

| Files | Change |
| --- | --- |
| `src/tracking-state.{hpp,cpp}` | State machine, reason/status snapshot, command validation |
| `src/target-selector.{hpp,cpp}` | Candidate scoring, lock identity, ambiguity and reacquisition rules |
| `src/crop-geometry.{hpp,cpp}` | Normalized region/crop math, safe-wide crop, image-bound clamping |
| `src/motion-controller.{hpp,cpp}` | independent pan/zoom dead zones, slew/acceleration limits, smoothing |
| `src/face-tracker-manager.*` | Adapter that publishes candidates; remove policy/crop ownership gradually |
| `src/face-tracker.*` | OBS-only capture/render adapter; source isolation and runtime resolution updates |
| `ui/sanctuary-style.{hpp,cpp}` | Small Qt token/palette/style helpers from `DESIGN.md` |
| `ui/sanctuary-control-dock.*` | Status-driven volunteer dock; reuse existing preview safely |
| `data/locale/en-US.ini` | All operator strings and status explanations |
| `tests/offline/*` | Engine unit tests and prerecorded-video scenario runner |
| `.github/workflows/*` | arm64/Intel macOS, Windows x64, Linux x86_64 build/package/test matrix |

## Cross-platform build plan

1. Install full Xcode on the arm64 developer Mac, Ninja, OpenBLAS, and the OBS
   SDK/release bundle matching the supported OBS release; initialize submodules.
2. Run the current upstream CMake/Ninja build with `CMAKE_OSX_ARCHITECTURES=arm64`,
   load the unmodified plugin in a matching OBS installation, and retain logs.
3. Add a Windows x64 GitHub Actions job using the OBS development environment,
   MSVC, submodules, dlib models, package creation, and plugin-load smoke test.
4. Validate the packaged DLL in a clean Windows OBS install with a real UVC
   camera and prerecorded media. Record OBS version, Windows version, driver,
   GPU, and logs. CI compilation alone is insufficient.
5. Keep macOS Intel and Linux x86_64 as best-effort supported builds; package
   only configurations with matching OBS/Qt ABI and test receipts.

## Test and acceptance criteria

The offline harness consumes normalized detections (and optionally recorded
video) so policy and crop tests are deterministic. Required scenarios: a
walking speaker, multiple people, brief occlusion, exit/re-entry, projected
faces, low light, input resolution change, camera loss, repeated scene changes,
and an eight-hour soak.

Pass criteria: no crop outside image bounds; no automatic target swap on an
ambiguous candidate; FROZEN/MANUAL never update crop; LOST reaches safe wide
once after its configured grace period; stable reacquisition meets its dwell
threshold; designated-source commands leave unrelated OBS sources/scenes
unchanged. For each fixture, report target switches, center/zoom jitter,
latency, CPU, memory trend, dropped/render frames, and state transitions.

## Prioritized milestones

1. **Validation gate:** successful untouched arm64 build/load and Windows x64
   CI build/package/load receipt.
2. **Engine seam:** normalized geometry and unit tests, no behavior change.
3. **Safety state machine:** LOCKED/SEARCHING/LOST/FROZEN/MANUAL and commands.
4. **Target continuity:** selection, ambiguity hold, region filtering, recovery.
5. **Motion quality:** independent smoothing, limits, safe-wide fallback.
6. **Operator UI:** native setup properties, Qt token layer, live dock.
7. **Harness and field validation:** fixtures, metrics, long-running and
   supported-platform acceptance report.

Each milestone must be independently reviewable, preserve GPL attribution, and
avoid a detector dependency replacement unless benchmark evidence justifies it.
