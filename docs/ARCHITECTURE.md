# Architecture

## Scope

The runtime executes the ARM32 Android engine from Minion Rush 1.8.1g on Apple
ARM64 platforms. Guest code is translated into ARM64 blocks at build time.
There is no runtime JIT, interpreter, Android virtual machine, or online
backend. The current Unity-based Minion Rush client is outside the scope of
every module.

The system has four layers:

1. validated original engine and game data;
2. deterministic A32 and Thumb to ARM64 block translation;
3. portable guest runtime and Android API shims;
4. separate macOS and iOS/iPadOS platform implementations.

## Build pipeline

1. `tools/validate_source.py` validates source and project structure.
2. `tools/configure_graphics.py` generates the host graphics header from
   `config/graphics.json` and validates the game profile.
3. `tools/validate_assets.py` verifies the engine hash, JPK directory entries
   and CRC values, JSON files, SQLite database, and offline-event evidence.
4. `a64-compiler` loads the ELF engine and emits static ARM64 code, block maps,
   and game-symbol bindings.
5. `game_code.S` embeds the generated code in the Mach-O `__MRCODE` segment.
6. The platform build links the portable runtime with one Apple platform layer.

`tools/project_common.sh` owns the shared build source list.
`config/source_manifest.txt` lists maintained public text files. macOS and iOS
use the same portable sources and cannot diverge into separate runtime variants.

`ios/Deployment.xcconfig` owns the iOS/iPadOS 17 deployment minimum for every
Xcode target and the native build script. Newer UIKit calls must pass the
compiler's availability checks; the SDK version is not the deployment minimum.
The Simulator matrix selects one installed minor release per OS family and
checks device support before creating temporary phones and tablets. Explicit
family arguments fail when any requested runtime is missing. Exact version
arguments select the version advertised by `simctl`; result labels include that
version so two releases in one family cannot overwrite each other's artifacts.

## Module ownership

The native macOS installer is a client of the build and asset tools, not another
runtime layer. Its UI state and bounded process bridge live in `installer`;
`tools/installer_backend.py` handles local workspace orchestration. Import and
validation share `asset_contract.py` fingerprints and the same validation API.
Signing-input validation shares `tools/signing.py` and the UI contract's rules.
The public UI contract and Windows backend boundary are documented in
`docs/INSTALLER.md`.

| Module | Responsibility |
|---|---|
| `a64_compiler.c` | Static A32 and Thumb block translation |
| `a64_runtime.c` | Generated-block dispatch |
| `elf_loader.c` | ELF loading, symbols, and relocation |
| `guest_runtime.c` | Guest clock and engine-specific runtime corrections |
| `guest_threads.c` | Cooperative guest pthread scheduler |
| `shim_libc.c` | libc, file system, and memory APIs |
| `shim_zlib.c` | zlib imports |
| `jni_bridge.c` | JNI surface required by the engine |
| `shim_gl.c` | GLES calls and render-target handling |
| `shim_audio.c` | OpenSL ES to CoreAudio bridge |
| `offline_mode.c` | Local policy for retired online services |
| `offline_events.c` | Local weekly-event catalog |
| `safe_area.c` | Safe-area adaptation for anchored engine UI |
| `window_macos.m` | AppKit window, input, and display link |
| `window_ios.m` | UIKit scene, touch, CoreMotion, and display link |
| `MRAppDelegate.m` | iOS application lifecycle and engine thread |

Each module owns its state. Other modules access that state only through
functions declared in headers. The larger translator and shim files each
implement one cohesive instruction-set or ABI boundary. Apple dependencies
belong only in the matching platform file.

On iOS, UIKit supplies the current top and bottom safe-area insets at runtime. The portable
safe-area adapter walks the interface tree and moves compact upper UI groups
and top-anchored vertical containers as complete layout units. A moved viewport
loses the same amount of height, which keeps its lower edge and bottom controls
in place while its scrollable content starts below the safe area. Engine object
names identify the small set of lower gameplay controls that require an explicit
bottom anchor and the revive anchors that intentionally overflow the display.
Lower controls retain a width-proportional interior clearance. Direct siblings
at or below the revive anchor band move as one group, including passive text and
artwork that the engine does not retrieve through its named lookup API.
The result score lookup identifies its containing statistics group. The adapter
treats that group as indivisible: a top-aligned group moves together, while a
centered group retains its layout even when some labels fall inside the upper
anchor band. It never shifts the group's labels or values independently.
The existing UI lookup hook records those objects without adding another lookup
path. Hierarchical transforms remain intact, and projected 3D menu containers
receive matching compensation so their embedded 2D labels stay aligned. Menu,
shop, event, achievement, and gameplay UI therefore share one layout policy
while full-screen backgrounds, the 3D render surface, and centered UI retain
their original geometry.

The asset-independent safe-area test covers groups that cross the upper anchor
boundary, compact upper groups, changing insets, and repeated frames. The
`testResultScreenRemainsResponsive` UI test provides screenshot attachments for
visual inspection on a notched iPhone as well as an iPad; it requires a save with
the gameplay tutorial completed. Screenshots and personal saves are not source
artifacts and must not be committed.

## Static block translation

The translator discovers A32 and Thumb entry points from executable ELF ranges
and follows provable control-flow edges: direct branches, calls, return
addresses, jump tables, and EHABI metadata.

Every generated block uses the same AAPCS64 contract:

| Register | Value |
|---|---|
| `x0` | `mr_cpu *` |
| `x1` | Host address of guest memory |
| `w2` | Guest-memory size |
| `w3` | Instructions completed on the current direct chain |
| `w4` | Runtime instruction budget |

Directly reachable blocks are joined with A64 `B` instructions. Distant
targets share a veneer island. Every edge checks the instruction budget so the
cooperative scheduler regains control even from long or cyclic chains.

Offline hooks never receive direct block edges. The block table marks hook
addresses so one runtime dispatch path applies the offline policy.

The generated image contains the engine fingerprint. Code generated for a
different engine cannot start.

## Guest memory and imports

The ELF image and guest allocations share one contiguous guest address space.
Host pointers never enter the guest ABI; the guest uses 32-bit addresses.

Imports are resolved once during startup:

- data imports point into static libc-shim storage;
- supported function imports receive thunks;
- unknown imports receive counted zero-return stubs;
- engine-specific symbols come from the generated bindings header.

Invoking an unknown stub during a run is a failure in the final report.

## Guest threads

The guest pthread API uses a cooperative scheduler. Only one host thread
executes guest code, so guest state is never raced across CPU cores.

The scheduler selects runnable threads, handles timed waits, implements mutex
and condition-variable behavior, dispatches audio-buffer callbacks, and
enforces the instruction quantum.

The CoreAudio real-time thread never executes guest code and never waits for a
lock.

## Frames and time

The display link is the only source of windowed frames. The platform keeps at
most one frame notification pending, preventing ticks from accumulating during
a slow render and causing a catch-up burst.

The host separates two clocks:

- real monotonic time drives scheduler deadlines;
- display-link time drives rendering and guest frame time.

While a scene is active, waits may execute guest background work in short
intervals. The interruptible wait uses a shorter interval while that work is
making progress and backs off when the guest scheduler is idle. In the
background, the engine waits without a deadline, avoiding polling and
unnecessary CPU use.

`preferredFrameRateRange` is only a request to the operating system. Each
display-link timestamp determines the actual frame interval.

The engine receives one integer-millisecond delta per simulation update. A
single fractional accumulator alternates adjacent integer deltas so their
average matches the selected cadence, and applies the engine time scale before
quantization. Running at 60 Hz therefore changes update frequency without
changing world speed.

## Graphics

The guest calls the GLES 2 API. On iOS, the host creates only a GLES3 EAGL
context. There is no ES2 fallback, although the GLES2 call surface remains part
of the binary ABI.

The engine renders to an offscreen target. The host completes that render,
fits it to the drawable, presents the `CAEAGLLayer` renderbuffer, and restores
the engine target.

The iOS drawable is recreated after size changes. Framebuffer and renderbuffer
objects are deleted during shutdown. `CADisplayLink` configuration stays on
the main thread. Startup framebuffer readback and PNG encoding are restricted
to diagnostic launches and never run during ordinary gameplay.

Texture uploads use a three-slot PBO ring with fences. If every slot is busy,
the shim performs a direct upload. That fallback is measured and counted.

## iPhone and iPad interface

The application creates one application-role `UIWindowScene`. It does not
create a separate window for external-display roles.

The application declares portrait and landscape at the application level on
iPhone and all orientations on iPad so the intro can request landscape geometry
and iPadOS can resize its scene. During gameplay and menus, the game view
controller narrows that declaration to upright portrait on iPhone and both
portrait orientations on iPad. On iOS and iPadOS 27, the scene delegate returns
the same dynamic mask through
`supportedInterfaceOrientationsForWindowScene:`. The movie playback page
temporarily changes the mask, requests landscape scene geometry, and resizes the
engine surface through its existing renderer callback. Leaving the movie
restores the portrait mask, request, and surface.

A scene geometry update is a request and may be denied by an iPadOS windowing
mode. In that mode, the intro can need a physical device rotation before its
scene becomes landscape. The renderer therefore treats the engine surface
orientation as the content contract, independent of the scene geometry. When
the two orientations differ, the game view is rotated and aspect-fitted in the
scene. This keeps the movie landscape and the game portrait without the large
pillarbox bars caused by fitting an unrotated portrait view in a landscape
scene. UIKit converts touch locations through the same view transform, and
motion axes follow the logical content orientation. A landscape scene may
persist until the physical iPad returns to portrait because iPadOS does not
guarantee geometry requests. The controller narrows its supported orientation
mask to the current scene only
after it reaches an allowed orientation. This is the shared locking policy on
iOS/iPadOS 17 and later. On 26 and later the same state also drives UIKit's
explicit orientation-lock preference, with availability-checked notification
and state queries. A movie transition first releases the lock and refreshes the
mask before requesting geometry. On 17 and 18 the scene delegate's coordinate-
space callback drives layout; on 26 and later the effective-geometry callback
does so instead. Both call the same handler, and the older callback is ignored
on newer systems to avoid duplicate updates. Foreground activation also invokes
that handler, so geometry changes made while the app was inactive receive the
same layout and orientation request. The foreground test checks portrait content
when windowing denies a turn, then checks portrait window geometry after the
simulated device returns upright. During iPad gameplay the portrait scene
stays locked against landscape. Device
orientation notifications run only while the scene is active; when the iPad
reaches the opposite portrait direction, the game view rotates 180 degrees.
The logical top and bottom safe-area insets swap with that rotation.
The gameplay mask excludes landscape. Scene-size changes also resize the guest
render surface through `Renderer.nativeResize`, keeping the original short-side
resolution and debouncing intermediate window sizes. Differences of at most eight rendered
pixels are filled consistently in the view, framebuffer copy, and touch map;
larger differences keep aspect-fit to avoid cropping controls.
On iPadOS 17 and 18, `UIRequiresFullScreen` is required for UIKit to enforce the
controller's orientation mask. The information property list also sets
`UIRequiresFullScreenIgnoredStartingWithVersion` to `26.0`, as described in
Apple's TN3192. That key is available from 26.2, so 26.0 and 26.1 retain the
older full-screen behavior. Later systems ignore that behavior and use the
same dynamically sized scene and orientation-lock policy.
`viewDidLayoutSubviews` and the scene geometry delegate recalculate the
transform after each window-size change.

Touch coordinates use the inverse of the same aspect-fit and rotation transform.
The guest receives one active touch. Moving to the background completes any
pending touch with a release event. Accelerometer sampling follows the active
scene lifecycle and its axes are transformed for the logical content orientation.

## Audio

The guest OpenSL ES buffer queue writes into a lock-free PCM ring. The RemoteIO
Audio Unit render callback reads from that ring. The cooperative scheduler
executes callbacks for guest buffers after their samples are consumed.

The iOS audio session belongs to the platform layer. Shared audio-shim code has
no UIKit or AVFoundation branch. Session deactivation cancels pending activation
retries. Resumption follows the system recommendation: iOS/iPadOS 27 and later
use the activation-lifecycle notifications, while 17 through 26 use interruption
notifications and their resumption option. Both paths share activation and retry
handling; foreground, route-change, and media-reset recovery remain common.

## File system

iOS uses three separate locations:

| Location | Role |
|---|---|
| bundled `game/files` | Read-only base data |
| `Application Support/MinionRush` | Saves and writable overlay |
| `Caches/MinionRush/Logs` | Regenerable logs |

There is no old Documents path and no migration. File reads check the writable
overlay first, then fall back to the bundled base.

## Offline behavior

A declarative symbol list blocks calls to retired services, including the
connection-poller worker. Capability checks consistently report an offline
state. Every guest resolver and socket import fails inside the guest ABI and is
never bound to a host networking API. A normal smoke test fails if any of those
final guards are reached.

Weekly events are constructed from a local catalog whose identifiers and text
keys are verified against the original game data. Controls whose only action
targets a retired online service, including the original Customer Care route,
are removed by the same centralized UI policy.

No engine address is hard-coded in runtime code. Required addresses come from
validated ELF symbols through the generated `game_bindings.h`.

## iOS packaging

`tools/build_ios.sh` creates platform-specific static libraries for physical
ARM64 iOS and the ARM64 Simulator. The Xcode target selects the matching
library. `tools/package_ios.sh` copies the validated assets for a signed device
build, while `tools/test_ios_simulator.sh` packages the same assets into an
isolated unsigned Simulator build.

`PrivacyInfo.xcprivacy` declares no tracking and no data collection. It also
contains approved reasons for the file-timestamp and monotonic-time APIs used
by the application.

## Enforced invariants

- There is one graphics configuration.
- Each platform has one presentation path.
- Validators do not modify the trees they inspect.
- ZIP installation follows no links and cannot write outside its destination.
- The portable core imports no Apple framework.
- macOS and iOS builds share one portable source definition.
- There is no interpreter, JIT, migration, or compatibility wrapper.
- A build cannot start with a different engine image.
- Every public `MR_*` runtime switch is documented.
- Generated and diagnostic files are not part of the source tree.

## Validation levels

| Level | Command | Coverage |
|---|---|---|
| Static | `./test.sh --static` | Sources, project, tool tests, and assets |
| macOS build | `./build.sh` | Strict compilation and block generation |
| macOS smoke | `./test.sh` | Headless and windowed startup |
| iOS package | `./tools/package_ios.sh` | Device build, opaque white app-icon rendering, private staging, assets, and signing |
| iOS Simulator | `./tools/test_ios_simulator.sh` | Clean iPhone SE and iPad landscape intro entry, language-menu UI, Hungarian startup, portrait framebuffer, and gameplay orientation policy on both devices |
| iOS device | `./tools/deploy_ios.sh` | Installation on paired physical devices |

### Simulator compatibility

The Release matrix passed on September 26, 2026, using Apple Silicon, macOS 27.0,
and Xcode 27.0 (27A266a). Each runtime ran on an iPhone SE (2nd generation) and
an iPad (10th generation):

| Runtime version | Runtime build |
|---|---|
| 17.0.1 | 21A342 |
| 18.6 | 22G86 |
| 26.5 | 23F77 |
| 27.0 | 24A434 |

The run completed 24 UI tests and eight portrait-startup/PCM checks. Coverage
includes landscape intro entry, the engine language settings page, Hungarian
startup, gameplay rotation policy, and foreground orientation and audio recovery.
The unsigned device build also passed with a 17.0 deployment minimum. This does
not establish runtime coverage for every patch release, including 17.0, 26.0,
and 26.1, or replace the physical-device checks below.

The 17.5 Simulator crashed in Apple's OpenGL ES compiler at
`cvmsServerElementBuild`. A standalone GLES3 triangle app reproduced the same
crash without the game engine. Apple describes this failure as a
[Simulator-only issue](https://developer.apple.com/forums/thread/756598).
That runtime is not part of the passing matrix; pin `17.0.1` when reproducing
the iOS 17 tests on this toolchain. No private driver workaround is used.

Physical iPhone and iPad regression testing remains a release gate. Static and
desktop checks do not replace code signing or real GPU, memory, audio, thermal,
energy, and iPad window-management tests.
