# Minion Rush 1.8.1g preservation runtime for Apple platforms

This project preserves the historical Minion Rush 1.8.1g Android release. It
translates the original ARM32 engine into ARM64 blocks at build time and runs
them behind native macOS, iOS, and iPadOS platform layers. It does not emulate
Android and does not depend on retired online services.

This is not a port of the current Minion Rush client. Gameloft's May 20, 2025
update moved the current game to Unity with a new visual system and redesigned
game modes. The two codebases and data formats are incompatible and are not
combined here.

The public repository does not include the original engine, DLC, graphics, or
audio. Each user must install these assets from a lawfully obtained copy of
release 1.8.1g. See `docs/LEGAL.md`.

## Supported environment

- Apple Silicon Mac running macOS 14 or later
- Xcode 26 or later with the matching iOS SDK
- ARM64 iPhone or iPad running iOS or iPadOS 26 or later
- Python 3 and the Xcode command-line tools

The iOS target builds for physical ARM64 devices and Apple Silicon's ARM64
Simulator. Simulator tests cover clean iPhone and iPad startup, localization,
portrait rendering, and gameplay landscape rejection on both devices. Physical
hardware remains the release gate for CPU, GPU, memory, thermal, energy, audio,
and window-management behavior.

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
| `assets/lib` | Validated 1.8.1g ARM32 engine, stored locally only |
| `assets/game` | Original game data, stored locally only |
| `config/graphics.json` | Single graphics configuration |
| `config/localizations` | Public metadata for optional community languages |
| `localizations` | Local XLIFF 2.1 translations, excluded from Git |
| `config/source_manifest.txt` | Manifest of maintained public text files |
| `tools` | Build, packaging, installation, and validation tools |
| `tests` | Automated tool tests |

## Initial setup

Create the ignored `assets` directory from your own 1.8.1g release:

```bash
python3 tools/install_assets.py /path/to/your-1.8.1g-release --root .
./build.sh
./test.sh
```

The installer accepts only the pinned engine fingerprint and a complete,
uncorrupted game data set. Other Minion Rush versions are unsupported.

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

Run the isolated iOS 26 Simulator matrix:

```bash
./tools/test_ios_simulator.sh
```

The test creates temporary iPhone SE (2nd generation) and iPad simulators,
installs a Release build, verifies the same-session landscape-to-portrait intro
transition and Hungarian startup, checks both iPad portrait directions, rejects
gameplay landscape on iPhone and iPad, and checks the portrait menu framebuffer.
It removes every generated simulator and build directory when it finishes. Set
`MR_SIMULATOR_ARTIFACTS` to an empty directory path to retain the build logs and
captured screenshots. Post-intro menu tests reuse the engine-generated macOS
settings file. Override its path with `MR_SIMULATOR_SETTINGS` when the macOS data
root is elsewhere.

Set `MR_SIMULATOR_SKIP_INTRO=1` to run the menu and orientation checks without
the clean-install intro test when diagnosing a slow Simulator renderer.

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

The iPhone application keeps gameplay and menus in upright portrait. The iPad
supports upright and upside-down portrait during gameplay and menus, while both
landscape orientations remain excluded. The intro movie temporarily requests
landscape scene geometry and switches to a landscape render surface, then
restores the platform-specific portrait policy and surface when playback ends.
UIKit may deny programmatic orientation changes in an iPadOS windowing
mode. In that case, the view rotates the unchanged logical surface into the
current scene instead of displaying a portrait surface between large side bars.
Touch and motion coordinates remain aligned with the logical game orientation.
An iPadOS scene opened in landscape can remain landscape until the device is
turned to portrait; a scene geometry request is not a guaranteed rotation.
On iOS and iPadOS 27, the scene delegate also supplies the current orientation
mask through the scene-level API.
UIKit safe-area insets move top-aligned 2D interface groups and scrollable
viewports below an obscured display region. Viewport height is reduced by the
same inset. Named lower gameplay controls stay above the bottom safe area,
with a small width-proportional interior clearance, while intentional decorative
overflow remains clipped at the display edge. Composite revive controls move as
one layout group so their button, label, progress, and decoration remain aligned.
Scene rendering and full-screen backgrounds continue to use the complete display.
The controller locks only an orientation already reached by the scene. On iPad,
the portrait scene stays locked against landscape while the game view turns
180 degrees when the device reaches the opposite portrait orientation. UIKit
applies the same view transform to touch input, and motion axes follow the
displayed content. Device-orientation notifications run only while the iPad
scene is active. The app does not use the deprecated full-screen compatibility
mode. In the background, the display link and engine thread wait until the
scene becomes active again.

## Community localizations

Installed community languages appear as ordinary flag buttons in the engine's
initial-language and Settings pages. A matching system language is selected
automatically on first launch. The menu choice is stored in Application Support
and can be changed while the game is running. Built-in and community buttons
share one centered, automatically extending grid, and community flags use the
same rounded silhouette and inactive tint as the original controls. Only the
selected language uses its full flag colors and orange selection border.

The Hungarian pack is created locally from `localizations/hu.xlf`. Its game
strings remain outside Git with the other proprietary assets. XLIFF validation
and deterministic compilation are part of every build. See
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

## License

Copyright 2026 Minion Rush Preservation Runtime contributors.

The original source code, tools, tests, and documentation in this repository
are licensed under the Apache License 2.0. See `LICENSE`.

The source license does not grant rights to distribute the original game,
name, or assets. Resolve the required rights and App Store compliance
separately before any public binary or commercial release.
