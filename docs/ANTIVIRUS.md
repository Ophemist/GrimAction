# Antivirus false positives

Microsoft Defender may flag binaries built by this project. This page explains why, what the project does
about it, and what it deliberately does not do.

## What happened

On 2026-09-13 Defender blocked and quarantined `gdtpc_runtime_config_tests.exe` the moment the build script
ran it. That program is an offline unit test for the INI configuration parser: it reads text, checks
values, and exits. It opens no process and injects nothing.

| Field | Value |
| --- | --- |
| Detection | `Trojan:Win64/Injector.NA!MTB` |
| Threat ID | 2147954600 |
| File | `artifacts\releases\collision\<release>\gdtpc_runtime_config_tests.exe` |
| Reproducible | Yes. The build is byte-for-byte deterministic (`/Brepro`), so every rebuild of the same source is flagged the same way. |

The `!MTB` suffix marks a machine-learning (behavioral/heuristic) detection, not a signature for known
malware. This project really is shaped like an injector: it ships an external injector and a DLL that
installs a function hook inside Grim Dawn. Files built alongside it look similar to the classifier, so
it can misjudge even a test that does nothing of the kind.

## What this project does not do

The project never changes code, build flags, packing or file layout to make a binary stop matching an
antivirus classifier. Reshaping a binary to get past detection is antivirus evasion. In a project that
injects code into another process, that is exactly what you should be suspicious of. If a build is
flagged, the answer is one of the options below, chosen by the person who owns the machine.

## Options, in order of preference

1. **Report the false positive to Microsoft** at <https://www.microsoft.com/en-us/wdsi/filesubmission>
   (choose "Incorrectly detected as malware"). This fixes the problem for everyone, not just one PC.
2. **Build, then verify, then trust.** Every release directory has a `release-manifest.json` with SHA-256
   hashes. `scripts\Test-ReleaseManifest.ps1` confirms that the files are exactly what the build produced.
3. **Add a narrow Defender exclusion** for the folder where the build creates and runs its binaries, and
   nothing wider:

   ```powershell
   # Elevated PowerShell. Adjust the path to your clone.
   Add-MpPreference -ExclusionPath 'C:\path\to\gd-third-person-runtime\artifacts\releases'
   ```

   Remove it when you no longer build:

   ```powershell
   Remove-MpPreference -ExclusionPath 'C:\path\to\gd-third-person-runtime\artifacts\releases'
   ```

   Check what is currently excluded with `(Get-MpPreference).ExclusionPath`.

Do **not** turn off real-time protection, exclude the whole repository, exclude your downloads folder,
or exclude the Grim Dawn installation. An excluded folder is one Defender never scans, so anything placed
there later is unscanned too.

## The maintainer's setup

The maintainer's development machine has exactly one exclusion for this project, added 2026-09-13:
`gd-third-person-runtime\artifacts\releases`. Compile intermediates (`artifacts\build-intermediates`),
sources, scripts and the game directory stay scanned.

## Before a public release

- Submit every flagged release binary (runtime DLL, injector, test executables) to Microsoft as a false
  positive, and record the submission here.
- Publish the SHA-256 manifest next to the download, so users can verify what they got.
- Consider code signing. Signed binaries with a reputation history get far fewer machine-learning
  detections than unsigned ones.
- Tell users plainly that the runtime injects a DLL into Grim Dawn. Point them to this page, and never
  advise disabling their antivirus.
