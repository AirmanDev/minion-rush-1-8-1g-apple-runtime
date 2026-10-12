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

## For users of an authorized, reviewed Windows kit

1. Extract the provided portable Windows kit to a private directory.
2. Double-click run-native-ipa.cmd to open the Windows graphical builder.
   Drop one original ZIP onto the release area or select Browse. Choose an
   output IPA filename and folder, then select Create IPA. The window displays
   translation and packaging messages while the build runs. Cancel ends the
   local worker process and removes its dedicated private scratch directory.
   The command-line tool remains available as run-native-ipa.ps1.
3. For Hungarian, place the private hu.xlf catalog in
   MinionRush-Private-Languages/hu.xlf next to the portable Windows kit.
   The original game ZIP does not contain this separate community translation,
   and the public repository does not distribute it. A missing catalog fails
   the build rather than silently producing question-mark labels.
4. Choose the lawfully obtained original unmodified 1.8.1g ZIP in the app. On UTM, the original ZIP may stay in the Mac shared
   folder: the builder automatically stages a private copy into Windows TEMP.
   For the default UTM SPICE WebDAV share, the input is retrieved through the
   VM's loopback-only HTTP endpoint (127.0.0.1:9843), not the mapped Windows
   WebDAV drive, which can refuse to open a ZIP larger than 50 MB.
   There is no manual copying or need to change registry/WebClient limits.
5. Wait for the local ZIP verification, full AOT translation, and IPA assembly.
   The progress indicator is indeterminate because translation stages do not
   provide trustworthy percent-complete measurements. The status reports
   each backend stage without inventing percentages.
6. The resulting unsigned IPA is written to the exact location selected in
   the app. Prefer local Windows storage for performance. The command-line
   runner still defaults to Downloads and writes windows-native-ipa-report.txt
   in the kit directory. The IPA
   is **unsigned** and contains copyrighted game files. Do NOT publish it.
7. Sign it with a compatible trusted external Apple sideloading tool using
   your own signing identity, and install it on your iPhone/iPad.

There are no embedded private game data, Apple SDK files, Apple certificates,
provisioning profiles, or developer secrets in the portable kit. Python is
included as the official embeddable distribution (PSF license included).
Neither the Windows GUI nor the Mac installer handles Apple account passwords
for third-party sideloading tools.

**Sideloadly successfully signed and installed a Windows-generated unsigned
IPA in the 2026-10-12 user test on an iPhone running iOS 27. Signing refresh,
long-duration stability, and additional Windows/iOS combinations have not
been validated. No App Store submission is planned.**

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
previously verified Windows x64 offline AOT compiler executable, and the
official Python 3.14.7 Windows x64 embeddable ZIP. The latter is downloaded
directly from python.org and pinned to SHA-256:

d297e5ff019966817ad8502465176139f2d3d840fa4ed84b13bed399a6ab1f15

Then run:

    python3 tools/build_windows_kit.py \
      --python-embed-zip /private/python-3.14.7-embed-amd64.zip \
      --compiler /private/a64-compiler.exe \
      --template build/native-template/MinionRush-native-template-unsigned.zip \
      --output /private/MinionRush-Portable-Windows

The kit includes a Python 3.14.7 embeddable interpreter, the locally
compiled x64 AOT executable, this project's public source tree, and the
developer-built unsigned iPhone application template. The portable Windows UI uses built-in PowerShell 5.1 and Windows Forms, with
no network service or extra GUI runtime. It starts the already verified Python
backend using an argument array, not an interpolated command or shell script.
The portable Python
python314._pth uses isolated module loading, explicitly permitting only
python314.zip, the interpreter directory, and the kit's runtime/tools directory.
No site import or system-wide PYTHONPATH is enabled. The full extracted
Python interpreter and native-extension files have an additional SHA-256
inventory; this prevents accidental file mismatches relative to the kit's
manifest, but does not replace an independently authenticated release signature. Windows users require
neither Xcode, a Mac, a Python installation nor a C compiler. The input ZIP and optional community-language catalogs must be
provided privately by the end user. The original game ZIP provides the
launcher icon, but not the Hungarian translation.

A source-manifest-based inventory protects the local runtime copy against
accidental content mismatches. SHA-256 of the downloaded Python embeddable ZIP
is pinned at kit creation. The compiler and template have hashes recorded in
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
sound, introductory sequence, completed gameplay, and saves. This confirmation
comes from the user's physical-device test; it is not automated hardware
regression coverage. The original game/IPA remains private and has not been
uploaded to public releases.

Still required before any public Windows kit: review the copyright status of
source-derived code maps/bindings and legal terms for redistributing the
asset-free template, test the GUI and build on additional real Windows systems,
and validate long-duration stability plus sideload signing renewal and save
retention after a signing refresh. No public Windows kit or private IPA has been released.

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
