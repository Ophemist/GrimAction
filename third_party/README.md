# Third-party source

## Microsoft Detours

- Upstream: https://github.com/microsoft/Detours
- Version: `v4.0.1`
- Pinned commit: `e4bfd6b03e50de46b47abfbd1e46b384f0c5f833`
- License: MIT (`detours/LICENSE.md`)
- Contents: the upstream `src` and `include` folders plus the build makefiles; samples are omitted.
- Build: `scripts/Build-Detours.ps1`. Linked into the runtime DLL for its single camera-update hook.