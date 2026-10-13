# Code audit

Review date: 2026-09-27. Scope: the maintained public source, native runtime,
Apple platform layers, installer, build and packaging tools, tests, and docs.
Private assets were validated locally but are not part of the public repository.

## Builder audit - 2026-10-13

Scope: all maintained sources in the public manifest (164 entries at this
review), Mac SwiftUI/Windows Win32 interfaces, translator, iOS/macOS runtime,
Python tooling, shell pipelines, tests, build outputs, and relevant docs.
Private original ZIPs and translations were not modified.

- The Mac and Windows IPA flows share `prepare_native_aot.py`,
  `native_ipa.py` and `native_ipa_output.py`. The duplicate Windows
  destination-and-replacement implementation was removed. Both validate
  destinations before compilation, assemble into a temporary sibling, reject
  links or changed targets, and preserve earlier output after failures.
- Native color tokens now have one source, `config/builder_theme.json`.
  The checked-in Swift/C definitions are generated and checked by both
  native build scripts and CI. The project icon remains generated from the
  same Mac artwork on both platforms.
- Native Mac unsigned IPA creation now bypasses unnecessary public-source
  workspace snapshots. The legacy Xcode signed-device path still validates
  and caches those sources.
- The Mac private .app uses the SHA-256-pinned ARM64 CPython runtime with
  unused Tcl/Tk, pip, IDLE and developer-only components excluded. The
  bundled interpreter still builds the same valid Hungarian IPA as the
  earlier full runtime. The size decreased from about 74 MB to 56 MB.
- The maintained source and native Swift files were checked with the
  project formatters; a previous C-localization formatting issue and
  SwiftUI/test layout violations were corrected. C/SWIFT presentation
  remains platform native rather than duplicated UI implementation.
- The canonical generated unsigned iOS template is kept under a single
  filename, `build/native-template/MinionRush-native-template-unsigned.zip`.
  Versioned portable Builder staging outputs and obsolete derived test
  artifacts were identified for removal. The current portable Mac .app,
  Windows EXEs, verified compilers, source ZIPs, private translation and
  reproducible toolchain archives are preserved.

Validation: 147 Python unit tests (146 passed, one Windows-only UTM
drive-mapping test skipped on macOS), native SwiftUI selector/model/layout
tests, source-tree validation, `bash -n`, native Mac and both Windows GUI
builds, `clang-format` and `swift-format` checks, and a complete
portable-Mac original-ZIP-to-IPA run without external Python/Homebrew.
The generated IPA contains 1371 verified ZIP members including Hungarian
text resources. Windows CI also compiles and self-tests both Win32 GUI
variants and the embedded one-file launcher.

Not claimed: independent legal redistribution clearance, exhaustive
pixel-by-pixel visual approval, complete absence of semantic duplication,
or an exhaustive device/OS performance and security certification.
Window screenshot automation was blocked by the remote macOS WindowServer;
the SwiftUI functional/layout tests passed without screenshots.

## Corrections

| Finding | Correction |
|---|---|
| iOS library builds could recursively delete a caller-selected directory. | Removed the unused output-directory argument. Outputs are fixed under `build`; linked build roots are rejected. |
| Snapshot reuse trusted a directory name without checking its files. | Cached source content, executable permissions, and link safety are checked before reuse. Damaged snapshots fail without automatic repair or deletion. |
| IPA export could create its temporary file inside the signed app. | Destinations inside the app bundle are rejected before creating any archive. |
| Source validation and packaging parsed manifests differently. | Both use the canonical parser in `tools/common.py`, including ordering, uniqueness, path, and self-inclusion checks. |
| The Mac window used manual retain/release despite the documented ARC policy. | Enabled ARC in the Mac build and removed manual ownership calls. Display-link invalidation and explicit teardown remain. |
| The installer stored an unused runtime path. | Removed the response field and UI state; workspace orchestration stays in the backend. |

Regression tests reproduce the unsafe export, directory cleanup, and unchecked
snapshot cases before their fixes. Manifest tests cover malformed inputs.
The safe-area C test also runs with AddressSanitizer and UndefinedBehaviorSanitizer.

## Structure

The translator, guest ABI, scheduler, graphics/audio bridges, offline policy,
and safe-area adapter retain separate ownership. Both platform builds consume
the same portable source list. Engine addresses stay in generated bindings;
release fingerprints reject a different engine image.

The Mac installer delegates asset import and deployment to the existing tools.
The separate Windows GUI in `src/windows/ipa_builder.c` delegates ZIP validation,
AOT compilation and IPA assembly to `tools/windows_native_package.py`; it does
not duplicate those operations. Sideloadly remains an external signing and
installation workflow. The public manifest excludes private game data,
signing material and generated output.
Source validation checks English/ASCII conventions and duplicated native bodies;
these checks cannot prove the absence of every semantic duplication or defect.

## Windows extension reviewed on 2026-10-12

The Windows 11 ARM command-line build and physical iOS 27 gameplay were
verified separately before the native Windows GUI was added. The Windows
x64 executable now uses one Python backend; the shipped kit also contains an
asset-free iOS template and SHA-256 checked tools. Its game-derived block
map and bindings need legal review before public distribution. Compilation
and synthetic tests cannot establish GUI usability, signing renewal,
long-running stability or legal clearance. See `docs/LEGAL.md`.

## Verification

Host: Apple Silicon, macOS 27, Xcode 27. Commands:

```bash
python3 -B -m unittest discover -s tests
python3 -B tools/validate_source.py
./tools/format_source.sh --check
bash -n ./*.sh tools/*.sh
./build.sh
./tools/build_ios.sh --platform device
./tools/build_ios.sh --platform simulator
./test.sh 30 120
./tools/package_installer.sh
```

The final Python suite has 83 passing tests. Both iOS engine libraries compile
with the shared iOS 17 deployment minimum. The final Mac smoke test passed both
headless and windowed startup, render-target, audio, and offline checks. The
unlocked windowed run completed 410 engine updates with no block faults or audio
underruns. Its engine time step averaged 16.67 ms at time scale 1.0. The earlier
locked-session run hit its watchdog; the unlocked rerun completed successfully.
This short startup check is not a sustained frame-rate or thermal benchmark.
The installer package checks cover Swift process/model/native controls, the
public-file allowlist, ZIP contents, executable permissions, and strict signature
verification after extraction.

Clang static analysis covers all C/Objective-C translation units for macOS,
iOS devices, and the ARM64 Simulator, including the iOS app delegate. No analyzer
diagnostics remain in those checked targets. Render resolution and frame-rate
settings are unchanged.

## Limits

This review is not a certification of production readiness or a proof that
every gameplay path is correct. No fresh physical-device installation, OS-runtime
matrix, clean-Mac Gatekeeper check, or Windows Sideloadly cycle was performed.
Compiling for iOS 17 is not equivalent to running on every supported OS version.
Long-session thermal behavior and gameplay require separate device measurements.
The proprietary engine and its original data are outside this source audit.
