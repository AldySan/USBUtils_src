# USBUtils design direction

Owner: AldySan. First recorded 2026-11-10 from the owner's stated direction
(antislop audit 001, finding 5). This file is the source of design truth for
UI work. `antislop.md` (in `anti-slop/`) is the filter applied on top of it.

## Identity

USBUtils is a quiet, trustworthy desktop tool for non-technical users who
want their PlayStation 2 games on a USB drive. The brand promise is
**comfort and ease**: the user should always feel in control, never lost,
never lectured.

Soul: comfort and ease.

## Style

Modern minimalist, clean, professional.

- Native Qt Widgets look on every platform. No skins, no gradients, no glow.
- System font. Hierarchy comes from weight and spacing, not extra typefaces.
- Calm color: neutral surfaces plus one semantic set (green ok, amber
  warning, red error) used only for real states, never as decoration.
- Plain, friendly English copy. Short sentences. Say what an action does
  before asking for confirmation.
- Comfort over density: generous padding, tooltips on every action.

## Dials

ENERGY 1 / RHYTHM 1 / MOTION 1 (calm, uniform, quiet).

Rationale: the tool manages irreplaceable user data (game installs, drive
repairs). A calm, predictable interface reads as safe; a busy one reads as
risky.

## Non-goals

- No branding experiments on top of the native widget set.
- No decorative animation. Progress indication is the only motion.
