# Minion Rush 1.8.1g preservation runtime for Apple platforms

This project preserves the historical Minion Rush 1.8.1g Android release. It
translates the original ARM32 engine into ARM64 blocks at build time and runs
them behind native macOS, iOS, and iPadOS platform layers. It does not emulate
Android and does not depend on retired online services.

This is not a port of the current Minion Rush client. Gameloft's May 20, 2025
update moved the current game to Unity with a new visual system and redesigned
game modes. The two codebases and data formats are incompatible and are not
combined here.

The public repository and generic desktop Builders do not include the original
engine, DLC, graphics, audio or any completed game IPA. Each user supplies
their own lawfully obtained release 1.8.1g ZIP. Conversion and IPA assembly
occur locally on that user's computer. This tool is independent of Gameloft
and does not authorize otherwise restricted copying or adaptation. See
`docs/LEGAL.md`.

## Supported environment

- Apple Silicon for the native Mac runtime and Builder.
- macOS 26.6 or later for the Builder and current Xcode 27 toolchain. The
  separately compiled macOS game runtime targets macOS 14 or later.
- Windows 11 x64 or Windows 11 ARM (x64 emulation) for the portable
  Windows Builder. The original game ZIP must be supplied by the user.
- iOS/iPadOS 17 or later for the resulting ARM64 application.
- The portable Mac and Windows Builders need neither Xcode nor a separately
  installed Python runtime to produce an **unsigned IPA**. Signing is separate:
  the optional Mac Xcode route uses an Apple Account and device provisioning;
  Windows users can sign and install with Sideloadly.
- Source development and native Apple signing still require the appropriate
  SDK, developer tools and local Python interpreter.

The [Releases](https://github.com/AirmanDev/minion-rush-1-8-1g-apple-runtime/releases)
include the standalone Mac and Windows Builders from v1.2.3 onward.
Version 1.2.6 prevents the AOT compiler's normal diagnostic output from
being misinterpreted as a Mac UI protocol error. It does not change generated
ARM64 instructions, game assets or installed save data.
Version 1.2.5 fixes the native macOS device and signing team dropdown
appearance in system Dark Mode without changing game conversion.
Version 1.2.4 makes the shared app version visible in both desktop interfaces,
improves paired-device selection and refresh on Mac, and preserves the
optional Magyar pack. The 1.1.0 Xcode-only importer remains a separate release.
Its generic unsigned iOS template is compiled without the original game,
precomputed game address tables, game assets, or the private Hungarian XLIFF.
A separate target-only Hungarian JSON pack is included, with no original
English source strings, and is compiled using the user's original ZIP.
The desktop Builder generates the entire user-specific AOT profile and code
before assembling the unsigned IPA. This reduces risks associated with
distributing game-derived data but does not grant rights to a game's port.
See [release scope](docs/RELEASE_SCOPE.md) and [legal context](docs/LEGAL.md).

## Runtime test status

The distribution target is direct GitHub download: the Mac Builder is
ad hoc signed and the Windows EXE is unsigned. No Developer ID,
notarization or Authenticode signing is planned. On some Macs, the user
can explicitly approve the application in Privacy & Security; managed
systems may prevent it. Signing an iOS IPA for device installation is
separate and still required.

Version 1.2.3 finalizes third-party notices, independent desktop branding
and bundled source-stripped language packs. It does not modify the iOS engine,
loading overlay or ARM64 compiler.
The portable application is titled "Unofficial IPA Builder", with Minion Rush
1.8.1g named solely as the supported original game. Source-controlled and
bundled MinGW-w64, Python and GCC runtime notices are checksum-checked. The 1.2.2 working baseline
remains available privately for comparison.

Version 1.2.1 fixed a black screen caused by the missing
`MR_RUNTIME_PROFILE` setting in the iOS entry point. The owner confirmed that an IPA built with
1.2.1 started on a physical iPhone.

Version 1.2.2 adds a startup overlay with game-data, graphics and engine
initialization messages. It closes after the first rendered frame rather
than a fixed delay. The overlay and gameplay rendering were tested on the
iOS 27.0 Simulator; the profile-based runtime was also tested on iOS 18.6.
The owner confirmed the 1.2.2 loading overlay and gameplay on an iPhone.
The owner additionally confirmed gameplay, audio and preserved saves on an
iPhone 17 Pro. Clean-device installation and broader device/OS coverage remain
separate tests.

The iOS target builds for physical ARM64 devices and Apple Silicon's ARM64
Simulator. Simulator tests cover clean iPhone and iPad startup, localization,
portrait rendering, iPad upside-down portrait, gameplay landscape rejection,
and foreground recovery on both devices. Physical hardware remains the release
gate for CPU, GPU, memory, thermal, energy, audio, and window-management behavior.

The deployment minimum is defined once in `ios/Deployment.xcconfig` and shared
by the app, UI tests, and native engine build. The compatibility target includes
iOS/iPadOS 17, 18, 26, and 27. API availability is a build error. An installed
Simulator runtime is required to claim runtime validation for that OS family;
compiling for iOS 17 alone does not validate its runtime behavior.
See the [executed compatibility matrix](docs/ARCHITECTURE.md#simulator-compatibility)
for tested releases and Simulator limitations.

The original engine uses OpenGL ES. The iOS port therefore retains one
EAGL/GLES3 compatibility path. There is no ES2 fallback. A native Metal port
would require a separate renderer rewrite.

## Offline execution

The host application has no networking framework or host socket path. The
retired connection worker and online service entry points are disabled, all
connectivity checks report offline, and every guest resolver and socket import
fails inside the guest ABI. Weekly events are generated locally from validated
game data. The complete smoke test fails if a final network guard is reached.

## Repository layout

| Path | Purpose |
|---|---|
| `src/native` | ARM32 to ARM64 translator, runtime, and platform layers |
| `ios/MinionRush.xcodeproj` | iPhone and iPad application project |
| `ios/MinionRush` | UIKit entry point and public asset catalog metadata |
| `installer` | Native macOS ZIP importer and device installer with IPA export |
| `config/installer_ui.json` | Shared installer labels, layout order, and signing rules |
| `assets/lib` | Validated 1.8.1g ARM32 engine, stored locally only |
| `assets/game` | Original game data, stored locally only |
| `config/graphics.json` | Single graphics configuration |
| `config/builder_theme.json` | Shared Mac/Windows Builder color palette |
| `config/native_profile.json` | Single native AOT profile and code-slot contract |
| `config/localizations` | Public metadata for optional community languages |
| `localizations` | Local XLIFF 2.1 translations, excluded from Git |
| `config/source_manifest.txt` | Manifest of maintained public text files |
| `tools` | Build, packaging, installation, and validation tools |
| `tests` | Automated tool tests |

## Builders and installation

The Mac and Windows applications share the desktop-only ARM32/Thumb to
ARM64 compiler and the generic iOS template ABI. A user supplies their own
original ZIP and selects an unsigned IPA destination. The Builder generates
the native ARM64 code, block map, engine bindings, and game assets locally.
The phone reads a validated address profile and executes the already compiled
ARM64 blocks; it never translates or JIT-compiles original instructions.

**Mac portable Builder (1.2.6):** self-contained, ad hoc signed arm64
`.app` with SwiftUI, pinned Python, host AOT compiler and unsigned generic
iOS template. No Xcode is required to make an unsigned IPA. The optional
Xcode signing/install route is separate. See [Mac Builder](docs/INSTALLER.md).

**Windows portable Builder (1.2.6):** self-contained Windows x64 `.exe`
with native Win32 UI, embedded Python, host AOT compiler and the same
unsigned generic iOS template. Windows 11 ARM uses x64 emulation. Sign the
result with a separately installed signing tool. See
[Windows Builder](docs/NATIVE_WINDOWS.md) and
[signing guide](docs/INSTALLER_WINDOWS.md).

The portable releases contain no original game, precomputed executable
profile, or original English game text. Both Builders include a target-only
Hungarian translation pack, which is compiled locally into a separate game
language table using the user's own source ZIP. Additional community packs
use the same framework. The rights to redistribute third-party translations
must be considered separately from the original game binaries.

The previously published 1.1.0 Xcode-only Mac application remains a separate
download. Build the asset-free Mac installer with `./tools/package_installer.sh`;
the full portable Mac Builder is packaged using
`tools/build_portable_mac_builder.py`. Unsigned IPA creation is disabled
in the separate asset-free Xcode-only installer.

The 1.2.3 Mac and Windows Builders passed unsigned IPA creation from the
original ZIP. The Windows test used the native GUI in a Windows 11 ARM VM
with x64 emulation. IPA contents and checksums were checked separately.
See [release scope](docs/RELEASE_SCOPE.md) for the test record.

The identified Python components and GCC runtime-license texts are included
in the candidate and checked against pinned digests. Residual user-side
transformation rights and release-decision considerations are tracked in
[third-party and release gates](docs/THIRD_PARTY_AND_RELEASE_GATE.md).

The 1.2.3 binaries must not be described as legally cleared or fully
device-tested until the outstanding rights and physical-device checks
described in [release scope](docs/RELEASE_SCOPE.md) are complete.

For the command-line workflow, create the ignored `assets` directory:

```bash
python3 tools/install_assets.py /path/to/your-1.8.1g-release --root .
./build.sh
./test.sh
```

The installer accepts only the pinned engine fingerprint and a complete,
uncorrupted game data set. Other Minion Rush versions are unsupported.
Imports are staged and validated before replacing existing assets; they do not
merge a new release into an older asset tree.

## Build and run on macOS

```bash
./build.sh
./run.sh
```

The build treats warnings as errors, validates sources and assets, generates
the ARM64 block image, and links the macOS executable.

Run with diagnostics enabled:

```bash
./run.sh debug
```

Diagnostic logs are written to `logs`. Normal runs do not create trace files.

## iPhone and iPad

Build a signed application:

```bash
./build.sh
./tools/package_ios.sh
```

Install on every paired physical device:

```bash
./tools/deploy_ios.sh
```

The deployment tool generates the translated blocks, builds the iOS engine
library and signed Release application, then installs it. Local Xcode
`xcuserdata` is excluded from the repository and cannot affect the build.

The scripts select the complete `/Applications/Xcode.app` toolchain even when
the active `xcode-select` path points to the standalone Command Line Tools.

Run the isolated matrix for all installed iOS Simulator families at or above
the deployment minimum:

```bash
./tools/test_ios_simulator.sh
# Require specific installed families; fail if any are missing.
./tools/test_ios_simulator.sh 17 18 26 27
# Pin an exact version reported by simctl instead of the newest in its family.
./tools/test_ios_simulator.sh 17.0.1 18.6
```

The test selects the newest installed minor release in each family and creates
temporary iPhone and iPad simulators supported by that runtime. Exact version
arguments select only that runtime; missing versions fail rather than being
skipped. It installs a Release build, verifies landscape intro entry (rotating
the simulated iPad when iPadOS denies a programmatic turn) and Hungarian startup.
It rejects upside-down gameplay on iPhone and landscape gameplay on both devices,
and checks foreground orientation, audio-session recovery, and the portrait menu
framebuffer.
Full intro completion is not automated because the 3D movie
advances too slowly under Simulator translation; the post-intro checks use an
engine-generated settings file. The test removes every generated simulator and
build directory when it finishes. Set `MR_SIMULATOR_ARTIFACTS` to an empty
directory path to retain the build logs and captured screenshots. Post-intro
menu tests reuse the engine-generated macOS settings file. Override its path
with `MR_SIMULATOR_SETTINGS` when the macOS data root is elsewhere.

Set `MR_SIMULATOR_SKIP_INTRO=1` to run the menu and orientation checks without
the clean-install intro test when diagnosing a slow Simulator renderer.
Set `MR_SIMULATOR_SAVE_FILE` to a local savegame path for the optional iPhone/iPad
gameplay screenshot test. The file is copied only into the temporary Simulator
data container and is not added to the repository or test artifacts.

Filter physical devices by name:

```bash
./tools/deploy_ios.sh iPad
```

The project uses Xcode automatic signing and contains no personal development
team. Supply your 10-character Apple Team ID for physical deployment and
override the bundle identifier when needed:

```bash
MR_DEVELOPMENT_TEAM=ABCDEFGHIJ \
MR_BUNDLE_ID=org.example.yourname.minionrush181 \
./tools/deploy_ios.sh
```

The standard application version is `1.8.1`. The custom `MRGuestVersion` key
in `Info.plist` preserves the exact historical label `1.8.1g`.

Each deployment updates the application in place under the same bundle ID. The
existing data container is retained, including completed-intro state, settings,
and saves. This is update preservation, not a legacy-data migration path. After
installation, the tool launches the game on each device and waits up to 120
seconds for initialized menu services, the local event, an audio signal, and a
captured non-black startup frame. Override the deadline with
`MR_STARTUP_TIMEOUT`. The framebuffer readback is enabled for this diagnostic
launch only; ordinary game launches do not capture or encode a startup image.

Gameplay and menus use upright portrait on iPhone and either portrait orientation
on iPad. The intro movie temporarily requests landscape scene geometry and
switches to a landscape render surface, then restores the portrait policy and
surface when playback ends.
UIKit may deny programmatic orientation changes in an iPadOS windowing
mode. In that case, the view rotates the unchanged logical surface into the
current scene instead of displaying a portrait surface between large side bars.
Window-size changes resize the game surface after a short debounce. Small
rounding differences are filled without side bars; larger aspect differences
remain aspect-fitted to avoid cropping controls. Touch and motion coordinates
remain aligned with the logical game orientation. An iPadOS scene opened in
landscape can remain landscape until the device is turned to portrait; a scene
geometry request is not a guaranteed rotation. On iOS and iPadOS 27, the scene
delegate also supplies the current orientation mask through the scene-level API.
UIKit safe-area insets move top-aligned 2D interface groups and scrollable
viewports below an obscured display region. Viewport height is reduced by the
same inset. Named lower gameplay controls stay above the bottom safe area,
with a small width-proportional interior clearance, while intentional decorative
overflow remains clipped at the display edge. Composite revive controls move as
one layout group so their button, label, progress, and decoration remain aligned.
The result statistics also remain one layout group: their labels and values
cannot receive different offsets when the group crosses the top-anchor boundary.
Scene rendering and full-screen backgrounds continue to use the complete display.
The controller narrows its supported mask to the reached scene orientation on
all supported versions. On iOS/iPadOS 26 and later it also requests the system's
explicit orientation lock. Geometry callbacks from the older and newer scene
APIs feed the same layout handler, without processing both on the same version.
While iPad gameplay holds the scene in portrait, active device-orientation
notifications rotate the game view between the two portrait directions.
On iPadOS 17 and 18, `UIRequiresFullScreen` lets UIKit enforce the controller's
orientation mask. `UIRequiresFullScreenIgnoredStartingWithVersion` is set to
`26.0`, so systems that support that key use the resizable scene policy instead.
The ignore key is available from iPadOS 26.2; 26.0 and 26.1 retain full-screen mode.
In the background, the display link and engine thread wait until the scene
becomes active again.
Foreground activation reapplies the same layout and orientation policy.

## Community localizations

Installed community languages appear as ordinary flag buttons in the engine's
initial-language and Settings pages. A matching system language is selected
automatically on first launch. The menu choice is stored in Application Support
and can be changed while the game is running. Built-in and community buttons
share one centered, automatically extending grid, and community flags use the
same rounded silhouette and inactive tint as the original controls. Only the
selected language uses its full flag colors and orange selection border.

The portable Builders include a versioned Hungarian target-only JSON pack,
without the private editable XLIFF or original English source text. Its 2,483
translated targets are verified against the user's source ZIP and compiled
into a separate game language table during IPA creation. Additional languages
use the same data-driven packaging and native menu. See
`docs/LOCALIZATION.md` for the translation workflow and the configuration-only
steps needed to add another banded flag and language without changing runtime
code.

## Data locations

The bundled `game/files` tree is a read-only base. Runtime state is written to:

- macOS: `~/Library/Application Support/MinionRush`
- iOS and iPadOS: the app sandbox `Application Support/MinionRush` directory
- `Caches/MinionRush/Logs`: runtime logs

Each macOS test uses a separate temporary overlay and fails if any bundled
source asset changes. There is no migration from an old Documents directory
and no parallel storage path. Physical-device deployment updates the installed
application in place and preserves its Application Support state.

## Tests

Run source, asset, and Python tests without building:

```bash
./test.sh --static
```

Run the complete macOS smoke test against the current build:

```bash
./test.sh
```

Override the headless frame count and windowed timeout:

```bash
./test.sh 120 60
```

Static validation covers:

- repository structure and essential Xcode target settings;
- duplicate source, platform boundaries, and Python and shell syntax;
- local application-icon provenance and format;
- engine SHA-256, JPK directory entries and CRC values, JSON files, and SQLite;
- ZIP extraction limits and protection from path traversal and symbolic links.

GitHub Actions runs the asset-free Python tests, shell syntax checks, and source
validator on every push and pull request. Contributors run builds that require
their local copyrighted game data.

A final iOS release still requires physical iPhone and iPad testing: startup,
background and foreground transitions, both iPad portrait directions, gameplay
landscape rejection, Stage Manager resizing, touch, accelerometer, audio,
saving, extended play, and thermal load.

## Reinstall assets

Only the official, unmodified 1.8.1g engine is accepted. The `assets` directory
is always constructed from scratch:

```bash
python3 tools/install_assets.py /path/to/the-release --root .
```

ZIP extraction has a size limit, rejects absolute and parent-traversing paths,
and rejects symbolic links. Installation also applies the single maintained
graphics configuration to the copied `profiles.json`, so a fresh asset install
and an existing development tree cannot diverge.

## Contributing

Read `docs/ARCHITECTURE.md` for module boundaries, `CONTRIBUTING.md` for the
development workflow, and `SECURITY.md` for private vulnerability reporting.
Issues and pull requests must not contain original game files, signing data,
device identifiers, or personal logs.

The asset installer extracts the original 1.8.1g launcher icon from the
verified APK into the ignored `assets/app-icon.png` path. During iOS packaging,
the Apple graphics pipeline removes the connected red outer frame and the old
card edge, composites the icon onto an opaque white background, and renders
Xcode's 1024x1024 sRGB input. The generated image exists only for asset catalog
compilation and is removed immediately afterward. The copyrighted icon is never
part of the maintained source tree.

## Graphics

`config/graphics.json` is the only graphics configuration source. The build
uses it to generate the C header and validate the game profile. There is no
parallel profile or runtime quality branch.

The current configuration requests a 1.25 render scale, 2x MSAA, 8x anisotropic
filtering, and a 60 Hz engine cadence. This keeps the target near the native
pixel count of the iPhone SE 2 and renders 30.6 percent fewer pixels than a 1.5
scale without device-specific branches. The operating system selects the
display refresh rate; the application provides only a preferred range.

## Environment variables

| Variable | Effect |
|---|---|
| `MR_WINDOW=0` | Run without a window |
| `MR_DATA_BASE=path` | Override the read-only macOS game-data base |
| `MR_DATA_ROOT=path` | Override the writable macOS data root |
| `MR_LANGUAGE=tag` | Override the selected BCP 47 language for this launch |
| `MR_LOCALIZATION_ROOT=path` | Override the compiled community-pack directory |
| `MR_HEADLESS_FPS=n` | Headless timing rate |
| `MR_THREAD_QUANTUM=n` | Guest-thread instruction quantum |
| `MR_DIAGNOSTICS=1` | Enable unified diagnostics and startup-frame validation |
| `MR_LOG_FRAMES=1` | Log frame statistics |
| `MR_SHOW_FPS=1` | Display performance data |
| `MR_TRACE=path` | Write a detailed trace |
| `MR_SHOT=path` | Write a screenshot |
| `MR_STARTUP_TEST_MS=n` | Startup-test deadline |
| `MR_JNI_LOG=1` | Log JNI calls |
| `MR_OFFLINE_LOG=1` | Log offline-service handling |
| `MR_GUEST_LOG=1` | Enable guest file logging |
| `MR_FRAMES=n` | Limit iOS diagnostic frames |

Build and deployment variables:

| Variable | Default |
|---|---|
| `CC` | `clang` |
| `PYTHON` | `python3` |
| `MACOSX_DEPLOYMENT_TARGET` | `14.0` |
| `MR_ASSETS_ROOT` | `assets` |
| `MR_BUNDLE_ID` | `org.example.minionrush181` |
| `MR_DEVELOPMENT_TEAM` | unset; required for physical deployment |
| `MR_IOS_DERIVED` | `/tmp/minion-rush-ios` |
| `MR_INSTALL_TRIES` | `3` |
| `MR_STARTUP_TIMEOUT` | `120` |

## Maintenance

```bash
./clean.sh
```

This removes generated build products, compiled language packs, diagnostic
logs, Python caches, and local Xcode user data. It does not touch source,
configuration, installed original assets, or private XLIFF translations.

Technical and historical references are listed in
`docs/TECHNICAL_SOURCES.md`.
See the [code audit](docs/AUDIT.md) for the reviewed boundaries, corrections,
verification results, and remaining test limits.

## License

Copyright 2026 Minion Rush Preservation Runtime contributors.

The original source code, tools, tests, and documentation in this repository
are licensed under the Apache License 2.0. See `LICENSE`.

The source license does not grant rights to distribute the original game,
name, or assets. Resolve the rights necessary for direct GitHub distribution
before publishing any binary. The Builder itself is not submitted to the
App Store.

The optional native Windows IPA template build uses the Xcode-only override
MR_IOS_LIB_DIR=ios-template; the standard builds retain MR_IOS_LIB_DIR=ios.
