# GrimAction

An over-the-shoulder, mouse-look third-person camera for **Grim Dawn** (Steam, 64-bit). You keep the game's own WASD or
controller movement, and F8 switches between the normal camera and third person.

- Mouse look with aim that follows the camera, plus automatic cursor release for menus, NPC dialog and Left Alt
- Camera collision, smooth zoom that doesn't shorten targeting reach, and shoulder views (F9)
- Nothing installed into the game folder: start it from the main menu, quit the game to remove it
- Fails closed: it refuses to load on any game build it wasn't made for

> Unofficial fan project, not affiliated with or endorsed by Crate Entertainment.

## Players

Download the latest zip from **Releases**, extract it anywhere, start Grim Dawn, and run **Start GrimAction** at the main
menu. The full guide (controls, settings, limits, troubleshooting) is [docs/PLAYER_GUIDE.md](docs/PLAYER_GUIDE.md), and it
ships in the zip as `README.md`.

GrimAction works by loading a DLL into the running game (DLL injection). Antivirus programs may flag it as a false positive;
see [docs/ANTIVIRUS.md](docs/ANTIVIRUS.md). Please don't disable your antivirus.

Player-facing release history is in [CHANGELOG.md](CHANGELOG.md).

Supported game build: Steam 24825149 (x64). When the game updates, GrimAction refuses to load until it's updated.

## Contributors

- [docs/DESIGN.md](docs/DESIGN.md): architecture, safety rules, how each feature works, known gaps
- [docs/BUILDING.md](docs/BUILDING.md): toolchain, build and package steps, testing rules

```text
src/native            C++ runtime DLL, pure models and their test executables
src/GdTpc.Injector    C# injector (load, initialize, logical stop, settings check)
src/GdTpc.ValidateBuild  offline supported-build validator
config/               supported build constants, example settings, prohibited releases
scripts/              build, validate, run, package
package/              player launcher scripts
extras/reshade/       optional aim-dot ReShade effect
third_party/detours   pinned Microsoft Detours 4.0.1 source (MIT)
```

Help wanted: right-stick pitch for controllers, hiding the game's hand cursor, and aim magnetism. See "What doesn't work
(yet)" in DESIGN.md.

## License

MIT, see [LICENSE](LICENSE). Third-party notices: [THIRD-PARTY-NOTICES.txt](THIRD-PARTY-NOTICES.txt).
