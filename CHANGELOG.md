# Changelog

All notable player-facing changes to GrimAction are recorded here.

## Unreleased

### Fixed

- Maximum virtual zoom no longer outruns the rendered world in short-view-distance areas; the far-plane performance cap now keeps a safety margin beyond the camera target.
- Native camera shake is suppressed only while virtual zoom needs an eye pull, preventing shake from replacing that pull and briefly throwing the camera far away.
- Switching from a gamepad back to keyboard and mouse now recaptures mouse look cleanly, without requiring a return to the main menu.
- Rift travel no longer leaves mouse look released after using the rift button with Left Alt held.
- Factions and Loot Filter now release and recapture the cursor like the other supported windows.

### Changed

- The optional dot cursor is much larger and outlined, so it is easy to see without the ReShade dot. It is centred exactly on the click point (shown as its lower-right quarter), and its size, outline and shape (quarter or offset round) are settings at the top of `Set-HiddenHandCursor.ps1`.
- Mouse look now follows Grim Dawn's own keyboard/mouse versus controller mode instead of continuing to reposition the OS cursor during controller play.
- Escape-menu detection now requires both of its native UI flags, preventing unrelated Alt-clicks from being mistaken for an open Escape menu.

### Known limitations

- At maximum zoom, looking almost horizontally can expose black beyond Grim Dawn's isometric level scenery. Zooming in slightly or looking downward avoids it.

## 0.1.0 - 2026-09-13

Initial public release.

- Over-the-shoulder third-person camera toggled with F8.
- Mouse look with camera-relative aiming, vertical look, and Left Alt cursor release.
- Automatic cursor release for the main supported panels and NPC conversations.
- Camera collision, proportional virtual zoom, shoulder switching with F9, and a far-plane cap.
- Optional ReShade aim dot and dot/blank replacement for the game's hand cursor.
- Build fingerprint validation, exact restoration on logical stop, reproducible release manifests, and fail-closed behavior on unsupported game builds.
