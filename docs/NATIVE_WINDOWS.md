# Experimental native Windows -> unsigned iPhone IPA support

This optional workflow supplements the existing Apple Silicon Mac installer and
native Xcode iOS workflow. It does not replace either. The goal is to let a
Windows user compile their own original Minion Rush 1.8.1g ZIP into an IPA with
native ARM64 machine code, without an interpreter, runtime JIT, or local Mac.

**Release status: Experimental. A public Windows release is blocked pending
physical iPhone signing/installation tests and legal review of the template.**

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
or translated ARM64 instructions belong in this template.

The Windows kit uses a locally shipped x64 AOT translator and Python standard
library to validate the user's original ZIP, generate machine code, verify
SHA-256 fingerprints of the code/map/bindings, fill only that reserved Mach-O
section, and package the user's verified original assets into an **unsigned**
Payload/MinionRush.app IPA archive. Windows 11 ARM supports running the
Windows x64 offline compiler through emulation. iOS will run the ARM64 result
natively after the IPA has been correctly signed.

## For users of an authorized, reviewed Windows kit

1. Extract the provided portable Windows kit to a private directory.
2. Double-click run-native-ipa.cmd.
3. Choose the lawfully obtained original unmodified 1.8.1g ZIP in the file
   selection dialog. On UTM, the original ZIP may stay in the Mac shared
   folder: the builder automatically stages a private copy from the Windows
   WebDAV/UNC share into Windows TEMP before reading it as a ZIP archive.
   There is no manual copying or need to change the UTM share settings.
4. Wait for the local ZIP verification, full AOT translation, and IPA assembly.
5. The resulting MinionRush-unsigned-*.ipa is written to the Windows
   Downloads directory (on the local Windows disk for better performance).
   The lightweight windows-native-ipa-report.txt is written to the kit
   directory so it can also be read through an UTM shared folder. The IPA
   is **unsigned** and contains copyrighted game files. Do NOT publish it.
6. Sign it with a compatible trusted external Apple sideloading tool using
   your own signing identity, and install it on your iPhone/iPad.

There are no embedded private game data, Apple SDK files, Apple certificates,
provisioning profiles, or developer secrets in the portable kit. Python is
included as the official embeddable distribution (PSF license included).
No installer/GUI in this project handles Apple account passwords.

**Sideloadly signing and installing an unsigned Windows-generated IPA has
not yet been verified on physical iPhones. No App Store submission is planned.**

## Developer: build the unsigned template on a Mac

First import the original release locally and run the normal existing build:

    ./build.sh

Then run the isolated optional unsigned iOS template build:

    ./tools/build_ios_template.sh

Default output:
build/native-template/MinionRush-native-template-unsigned.zip

The template builder uses Xcode's iPhoneOS SDK only on the developer's Mac,
builds the isolated build/ios-template library, creates an independently
generated neutral icon, turns off Xcode code signing, builds a separate
DerivedData tree, and rejects any original game assets/signatures and any
nonzero executable slot. It does not modify build/ios, the normal iOS target,
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
developer-built unsigned iPhone application template. The portable Python
python314._pth uses isolated module loading, explicitly permitting only
python314.zip, the interpreter directory, and the kit's runtime/tools directory.
No site import or system-wide PYTHONPATH is enabled. The full extracted
Python interpreter and native-extension files have an additional SHA-256
inventory; this prevents accidental file mismatches relative to the kit's
manifest, but does not replace an independently authenticated release signature. Windows users require
neither Xcode, a Mac, a Python installation nor a C compiler. The input ZIP
must be provided privately by the end user.

A source-manifest-based inventory protects the local runtime copy against
accidental content mismatches. SHA-256 of the downloaded Python embeddable ZIP
is pinned at kit creation. The compiler and template have hashes recorded in
kit-manifest.json. The runtime verifies hashes again when launched.

## Automated safety boundaries

- No overwriting existing output archives or operating outside the intended
  temporary working directory. Windows network ZIP sources are streamed into
  local TEMP using bounded 1 MiB buffers, because UTM's WebDAV implementation
  may reject the random seeks used by ZIP readers (Errno 22). Local NTFS ZIPs
  are used directly without an unnecessary copy. Scratch copies are deleted
  on success or failure; reserve sufficient free Windows VM disk space.
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

Still required: execute the portable ZIP-to-IPA builder end-to-end inside the
Windows VM, test external signing of its unsigned IPA, and install and run it
on a physical iPhone, verifying CPU/GPU/audio/gameplay and data preservation.
No public Windows kit or private IPA has been released.

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
