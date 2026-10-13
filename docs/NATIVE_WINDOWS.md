# Experimental native Windows -> unsigned iPhone IPA support

This optional workflow supplements the existing Apple Silicon Mac installer and
native Xcode iOS workflow. It does not replace either. The goal is to let a
Windows user compile their own original Minion Rush 1.8.1g ZIP into an IPA with
native ARM64 machine code, without an interpreter, runtime JIT, or local Mac.

**Release status: Experimental. Physical iPhone signing, installation,
gameplay, Hungarian text, saves, sound and the original icon have passed a
user-reported Windows 11 ARM test on 2026-10-12. Public distribution is still
blocked pending copyright review of the template and release readiness checks.**

## How the system works

The existing offline ARM32/Thumb -> ARM64 compiler generates four files from
the user's own official, unmodified 1.8.1g engine. Three of those outputs are
important for the iOS runtime:

1. minion_code.bin: native ARM64 machine-code blocks.
2. minion_map.inc: ARM32/Thumb entry points and corresponding block offsets.
3. game_bindings.h: fixed original-engine entry points and fingerprint.

The regular Mac/iOS build continues to embed the generated code and map at
link time. The additional Windows path uses a special **developer-built**
unsigned iOS Mach-O application template. It contains the same compiled host
runtime and block map, but reserves a file-backed executable __MRCODE/__text
section filled with zeros. No original game engine, artwork, DLC, audio, icon,
or translated ARM64 instructions belong in this template. No compiled app
icon catalog is included: after importing the user's ZIP, the Windows builder
generates correctly sized iPhone/iPad PNG icons from its validated original
app-icon.png and records them in CFBundleIcons. The final per-user IPA therefore
uses the original game's launcher artwork; the developer's template does not.

The Windows kit uses a locally shipped x64 AOT translator and Python standard
library to validate the user's original ZIP, generate machine code, verify
SHA-256 fingerprints of the code/map/bindings, fill only that reserved Mach-O
section, and package the user's verified original assets into an **unsigned**
Payload/MinionRush.app IPA archive. Windows 11 ARM supports running the
Windows x64 offline compiler through emulation. iOS will run the ARM64 result
natively after the IPA has been correctly signed.

The kit's prebuilt translator is verified by SHA-256 and executed directly
from its existing kit location. It is never copied into the private AOT
output or deleted by the preparation routine: Windows can keep executable
images locked briefly after the child exits (WinError 5). This also avoids
an unnecessary executable copy. The private output contains only original
assets, generated ARM64 data and the AOT manifest.

## Windows GUI release workflow

1. Download the Windows ZIP from the project's official GitHub Release after
   legal approval. Compare its SHA-256 to the published checksum and extract
   the complete folder. Do not run the EXE inside a ZIP preview.
2. Open MinionRush-Portable-Windows/MinionRush Builder.exe.
3. Drop one original 1.8.1g ZIP onto the window or select Browse ZIP.
   In the standard release builder, choose a locally accessible ZIP. The
   separately compiled UTM test builder recognizes an original ZIP next to
   the kit in the private shared folder, fills its path automatically, and
   uses the guest-local WebDAV transport to bypass the Windows file-picker
   size limit. If necessary, paste a full absolute ZIP path in the source
   field (Windows Explorer: Shift + right-click, Copy as path); enclosing
   quotes are accepted. This test-only behavior is not in the release EXE.
4. Select Save As and choose a writable folder and a new filename ending
   in .ipa. Existing IPA files are never overwritten.
5. Optionally select a private Hungarian hu.xlf catalog. This translation is
   not part of the original game ZIP. The builder can also use
   MinionRush-Private-Languages/hu.xlf next to the portable kit. The private
   IPA embeds the compiled language text, formatter and font metadata; the
   editable XLIFF is not put into the iOS app. The game already provides
   initial and in-game Settings language selectors for choosing installed
   languages, with persistent preferences. Switching among built-in languages
   and an included Hungarian pack does not require rebuilding.
6. Select Create IPA. Progress and the latest step appear in the
   window without freezing the user interface. Show details expands the
   diagnostics with the build phase, source, output, translation, transfer
   method, workspace path, exit status, error and the available raw output.
   Copy error copies the diagnostic summary, Copy full log includes the raw
   output, and Save log writes a local UTF-8 text report. Error details
   expand automatically on failure. Cancel terminates the child process
   group and cleans its private scratch files.
7. Open the chosen output folder. The IPA is unsigned, contains copyrighted
   game assets from your own ZIP, and is for private use only.
8. Sign and install it with a trusted external tool such as Sideloadly.
   The GUI does not collect Apple account credentials or sign IPA files.

The kit contains an unsigned game-asset-free iOS template, the native
Windows EXE, and a private Python runtime plus AOT compiler. Game assets,
original icon and ARM64 machine instructions are generated only after
the user imports their own original ZIP. No compiler, Python or Xcode
installation is required by the Windows user.

The former batch and PowerShell startup wrappers were removed. The GUI
starts the existing offline build engine with an explicit source ZIP,
chosen destination, and optional language catalog; there is only one
translation and IPA assembly implementation.

The release and private UTM test builder have distinct compile-time
configurations and version-3 kit manifests. Only the UTM test kit includes
the guest-loopback SPICE/WebDAV transport module, which targets
127.0.0.1:9843 for large shared ZIPs. The release kit excludes that module
and the release GUI does not auto-select UTM share paths. Local archives
use normal file I/O. The GUI uses short per-job Windows temporary paths
to avoid classic 260-character filesystem-path failures.
Successful and cancelled builds clean their private workspace. Failed
builds retain the workspace for investigation, accessible through Open temp
folder. This workspace can contain original copyrighted game files, so
never share it or a diagnostic log publicly and remove it manually after
troubleshooting. The Windows GUI's displayed log is capped at 4 MiB; it
explicitly reports when later output is omitted.

An x64 Win32 GUI can run on x64 Windows and on Windows 11 ARM through
Windows x64 compatibility. No administrator privileges are required.
Unsigned Windows EXEs can trigger SmartScreen warnings. Prefer a verified
code signing certificate and published checksums for final releases.

The previous command-line pipeline was verified by the user on an iPhone
running iOS 27 with original app icon, Hungarian localization, gameplay,
sound and saves. The new GUI and cancellation lifecycle need an additional
interactive Windows test before publishing it to all users.

## Developer: build the unsigned template on a Mac

First import the original release locally and run the normal existing build:

    ./build.sh

Then run the isolated optional unsigned iOS template build:

    ./tools/build_ios_template.sh

Default output:
build/native-template/MinionRush-native-template-unsigned.zip

The template builder uses Xcode's iPhoneOS SDK only on the developer's Mac,
builds the isolated build/ios-template library, disables the asset-catalog
app icon and code signing, builds a separate DerivedData tree, and rejects
any app icon, original game assets/signatures or nonzero executable slot. It does not modify build/ios, the normal iOS target,
or the existing Mac installer.

A normal iOS device build remains:

    ./tools/build_ios.sh
    ./tools/package_ios.sh

Only a **developer-owned, suitably reviewed asset-free template** may be
offered publicly, not a privately generated full IPA. Although the Mach-O
template contains no original machine-code instructions or original game
assets, the embedded static block map and binding addresses are derived from
the original game. Their redistribution requires independent copyright
review; no legal conclusion is implied by these technical checks.

## Developer: build a portable Windows kit

The build host needs the unsigned template from the preceding step, a
verified Windows x64 offline AOT executable, a native GUI EXE, and the
official Python 3.14.7 Windows x64 embeddable ZIP. The latter is downloaded
directly from python.org and pinned to SHA-256:

d297e5ff019966817ad8502465176139f2d3d840fa4ed84b13bed399a6ab1f15

Build the standard Windows x64 GUI on a Mac with MinGW-w64:

    ./tools/build_windows_gui.sh

Build and validate a release-profile kit (the default). UTM-specific
transport source and test-only GUI behavior are not shipped in this kit:

    python3 tools/build_windows_kit.py \
      --mode release \
      --python-embed-zip /private/python-3.14.7-embed-amd64.zip \
      --compiler /private/a64-compiler.exe \
      --gui-exe build/windows/MinionRush\ Builder.exe \
      --template build/native-template/MinionRush-native-template-unsigned.zip \
      --output /private/MinionRush-Portable-Windows

For private UTM VM testing, compile and package a separate test profile:

    ./tools/build_windows_gui.sh utm-test
    python3 tools/build_windows_kit.py \
      --mode utm-test \
      --python-embed-zip /private/python-3.14.7-embed-amd64.zip \
      --compiler /private/a64-compiler.exe \
      --gui-exe build/windows/utm-test/MinionRush\ Builder.exe \
      --template build/native-template/MinionRush-native-template-unsigned.zip \
      --output /private/MinionRush-UTM-Test

Create a release-profile candidate and checksum (no upload or GitHub release):

    python3 tools/package_windows_release.py \
      --kit /private/MinionRush-Portable-Windows \
      --output /private/MinionRush-Windows-Builder-candidate.zip

The private UTM test archive requires an explicit safety override:

    python3 tools/package_windows_release.py \
      --kit /private/MinionRush-UTM-Test \
      --output /private/MinionRush-UTM-Test.zip \
      --allow-test-profile

Never label or publish that archive as the standard Windows release.

The kit includes the native Win32 GUI, an isolated Python 3.14.7 runtime,
the verified x64 AOT translator, public runtime source, and the developer-built
unsigned iPhone application template. The portable Python
python314._pth uses isolated module loading, explicitly permitting only
python314.zip, the interpreter directory, and the kit's runtime/tools directory.
No site import or system-wide PYTHONPATH is enabled. The full extracted
Python interpreter and native-extension files have an additional SHA-256
inventory; this prevents accidental file mismatches relative to the kit's
manifest, but does not replace an independently authenticated release signature. Windows users require neither Xcode, a Mac, an installed Python runtime
nor a C compiler. The input ZIP and optional community-language catalogs must be
provided privately by the end user. The original game ZIP provides the
launcher icon, but not the Hungarian translation.

A source-manifest-based inventory protects the local runtime copy against
accidental content mismatches. SHA-256 of the downloaded Python embeddable ZIP
is pinned at kit creation. The GUI, compiler and template have hashes recorded in
kit-manifest.json. The runtime verifies hashes again when launched.

## Automated safety boundaries

- No overwriting existing output archives or operating outside the intended
  temporary working directory. Windows network ZIP sources are streamed into
  local TEMP using bounded 1 MiB buffers. UTM's mapped SPICE WebDAV drive
  can reject even the initial open of a 454 MB file (Errno 22) because of
  Windows WebClient file-size limits. For recognized UTM loopback WebDAV
  mappings only, the importer instead issues a direct HTTP GET to the guest's
  own 127.0.0.1:9843 SPICE service, without proxies or redirects. The
  result is size-checked before ZIP validation. Unrelated remote shares
  continue using the regular file API. Local NTFS ZIPs are used directly
  without an unnecessary copy. Scratch copies are deleted on success or
  failure; reserve sufficient free Windows VM disk space.
- Version-locked original 1.8.1g engine hash, asset content validation, bounded
  original ZIP extraction, path-traversal and symlink checks.
- Thin ARM64 MH_EXECUTE Mach-O only, exactly one file-backed executable
  __MRCODE/__text section, strict offsets/size, zero-slot verification.
- The code inserted must match the known template reference checksum.
  CRLF normalization is applied solely to compare generated text metadata.
- The unsigned template ZIP is checked for duplicate paths, encrypted entries,
  unknown files, signing data, and game-data paths.
- The user's original engine and assets never enter the public source release.
  Native code is emitted only as private per-user output.
- The source icon is validated against the original game's SHA-256 digest.
  The app's own PNG icon variants are created after private ZIP import,
  without Xcode, from the validated source artwork. Template-provided icon
  catalogs or icon metadata are rejected to prevent duplicate icon sources.
- No Android emulation, no iOS JIT, no remote upload or host networking.
- iOS signature must be created only **after** the per-user IPA is assembled;
  modifying a signed executable invalidates the signature.
- Users must not share their IPA, ZIP, local logs, signing identity, or device
  identifiers on GitHub issues or public cloud services.

## Executed verification

On 2026-10-12:

- Windows 11 ARM VM started the x64 compiler without Windows SDK/Clang.
- The complete original game engine translated to 938598 A32 and 4146 Thumb
  native blocks. ARM64 code digest matched the Mac reference.
- All four generated outputs matched the Mac references (after normalizing
  Windows CRLF in the three text artifacts).
- The Mac Xcode unsigned template build succeeded, producing a 164353244-byte
  all-zero executable slot in __MRCODE/__text.
- The Mac end-to-end assembly generated an unsigned IPA (376453529 bytes,
  1367 ZIP members). The inserted native ARM64 code matched the original
  compiled code SHA-256 exactly.
- A **local ad-hoc signature** of that IPA's app bundle passed
  codesign --verify --deep --strict, confirming the Mach-O can be signed
  structurally. This ad-hoc signature is NOT sufficient for an iPhone.

The same day, a complete Windows 11 ARM UTM end-to-end test succeeded:
the portable kit read the original game ZIP over local SPICE WebDAV, compiled
native ARM64 code, included the 2483-entry private Hungarian localization,
generated the app's icon PNGs from the owner's original ZIP, and produced an
unsigned IPA. The shared Windows report recorded PASS, an output size of
447664313 bytes, and SHA-256:

2a4702ff8627d67f6b0396b90ad4523a35e6e345410cc9c7271654bb063cb4fd

The user then confirmed successful Sideloadly installation on an iPhone
running iOS 27, working original app icon, Hungarian labels instead of ???,
sound, introductory sequence, completed gameplay, and saves. The user
subsequently confirmed repeated extended play without observed stability
problems. This is user-reported physical-device testing, not automated
hardware regression or comprehensive device coverage. The original game/IPA remains private and has not been
uploaded to public releases.

Still required before any public Windows kit: review the copyright status of
source-derived code maps/bindings and legal terms for redistributing the
asset-free template, perform an interactive end-to-end test of the new Win32
GUI on Windows, test additional real Windows systems, and validate
sideload signing renewal and save retention after a signing refresh. No public Windows kit or private IPA has been released.

## Further reading

- Apple Mach-O segments:
  https://developer.apple.com/documentation/kernel/segment_command_64
- Apple code-signing behavior:
  https://developer.apple.com/library/archive/technotes/tn2206/
- Modern iOS code signature requirements:
  https://developer.apple.com/documentation/Xcode/using-the-latest-code-signature-format
- Python Windows embeddable package:
  https://docs.python.org/3/using/windows.html
- Sideloadly official site:
  https://sideloadly.io/
- Legal boundary: docs/LEGAL.md
