# FaithfulFrame design system

## Purpose

FaithfulFrame is an OBS-native control surface for a volunteer operating one
fixed sanctuary camera. It makes the current tracking state and the safe escape
actions obvious before exposing tuning. This is a focused product layer, not a
general-purpose UI kit.

## Sources and implementation boundary

The design is informed by Astryx's token-first theming, role-based hierarchy,
intentional spacing, and complete interaction-state guidance. It is implemented
with Qt Widgets and OBS property controls: no Astryx React components, browser
runtime, or Electron application is included. Qt's platform theme remains the
foundation; FaithfulFrame applies a small semantic token layer on top.

## Product principles

1. **The stream stays safe.** Freeze, wide framing, and manual override are
   always available and never change unrelated sources or scenes.
2. **One camera, one clear owner.** Every dock displays the selected source;
   selection is explicit and tracking is scoped to its dedicated output/filter.
3. **State is explained, not implied.** State text, an icon, and color are
   shown together. Color alone never communicates a condition.
4. **Calm over clever.** A stable crop is preferable to a fast but ambiguous
   switch. Motion honors system reduced-motion settings where Qt exposes them.
5. **Progressive disclosure.** Live controls first; setup next; tuning and
   diagnostics stay in Advanced Settings.

## Interface architecture

| Surface | Audience | Contents |
| --- | --- | --- |
| Live Control dock | Volunteer during service | Source, state, zoom, preset, compact preview, four safety actions |
| Source properties: Camera Setup | Operator before service | Source, resolution, region, crop preview, framing and configuration persistence |
| Source properties: Advanced | Technician | detector scale, motion parameters, loss/reacquisition thresholds, diagnostics |

The dock is native Qt and subscribes to the selected FaithfulFrame instance's
read-only status model. Properties are the source of truth for persistent
configuration. The dock sends commands only through that instance's command
interface; it does not enumerate, edit, or render another OBS source.

## Tokens

Values are logical pixels unless noted. Implement constants in
`ui/sanctuary-style.{hpp,cpp}` and derive colors from the active OBS palette.

| Category | Tokens |
| --- | --- |
| Spacing | `space-1=4`, `space-2=8`, `space-3=12`, `space-4=16`, `space-5=20`, `space-6=24` |
| Radius | `radius-sm=4`, `radius-md=8`, `radius-lg=12` |
| Controls | compact 28px, standard 36px, primary/safety 44px minimum height; 44×44px minimum pointer target where layout permits |
| Type | title 18px semibold; section 14px semibold; body 13px regular; label 12px medium; numeric/status uses tabular figures when available |
| Color roles | `surface`, `surface-raised`, `text`, `text-muted`, `border`, `focus`, `accent`, `success`, `warning`, `danger` |

Semantic colors are mapped to the active light/dark Qt/OBS palette at runtime.
Use `accent` only for the primary action/selection; use `danger` only for an
action that stops or discards tracking. State color combinations must meet at
least 4.5:1 for normal text, with a non-color label and icon.

## Interaction and accessibility

Every control has a visible text label or accessible name, meaningful tooltip,
keyboard focus, and logical tab order. Focus uses the shared `focus` token with
a visible 2px ring and is never removed. Buttons implement rest, hover, active,
focus, disabled, and busy states; disabled controls explain why in nearby text.
Destructive/safety actions retain confirmation only when delay is safe—Freeze
and Reset to Wide execute immediately and report the result in the state line.

Do not encode LOCKED/SEARCHING/LOST/FROZEN/MANUAL by color alone. Update the
accessible status name when state changes. Respect high-contrast palette roles;
avoid fixed dark/light colors. Animations are short and non-essential; when
reduced motion is requested, use instant state/crop UI transitions.

## Layout rules

Use the 4px grid. Group related controls through heading, alignment, and
proximity rather than nested cards. The dock has: status header; preview;
primary follow control; two-by-two safety control grid; preset row; disclosure
link to setup. One primary action appears per state: Auto Follow in FROZEN or
MANUAL, Freeze in LOCKED/SEARCHING, and Reset to Wide in LOST.

## Operator workflows

**Before service:** select the designated camera, verify detected resolution,
draw the stage region, choose framing/maximum zoom, save a named preset, and
verify the preview.

**During service:** confirm source and LOCKED state, use Auto Follow once,
Freeze when an unplanned person enters, Reset to Wide when tracking is
unreliable, and choose Manual Override to bypass automation. These actions
preserve the livestream and do not alter other sources/scenes.

**After a loss:** SEARCHING holds the last valid crop during the grace period;
LOST moves once to the configured safe wide crop. Only a stable, unambiguous
match can return to LOCKED automatically. Ambiguous candidates require Auto
Follow or target selection.

## Anti-patterns

- A dashboard of low-value telemetry during a live service.
- Decorative buttons that do not command the tracker.
- Nested cards, arbitrary spacing, or hard-coded theme colors.
- Auto-switching to the largest/newest face after a target is uncertain.
- Hiding emergency actions under Advanced Settings or a context menu.
- Describing compiled CI as field validation on Windows hardware.
