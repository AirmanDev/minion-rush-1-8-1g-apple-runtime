# Native installer

The Mac interface is titled "Unofficial IPA Builder" and identifies
Minion Rush 1.8.1g only as the compatible input. It is not affiliated
with or endorsed by the game's rightsholders.

The Mac installer is a native SwiftUI application for creating an unsigned
IPA from the user-owned original 1.8.1g ZIP. Its optional Xcode workflow
continues to sign and install on connected Apple devices. It is not the game,
an asset downloader, or a signing service.

## Requirements

- Apple Silicon, macOS 26.6 or later; macOS 27 is included in the target range
- For unsigned IPA creation, the portable Mac Builder includes a verified AOT
  translator and prebuilt unsigned iOS template; Xcode is not required at runtime.
- For the optional signed export/install, Xcode 27, an Apple Account signed
  into Xcode and a paired iPhone or iPad remain required.
- A legally obtained, unmodified 1.8.1g release ZIP containing its APK and data
- Space for extraction, validated assets, compiled code, and Xcode build products

The portable Mac .app includes its own arm64 CPython 3.14 runtime, the native
AOT translator and a generic unsigned iOS template. The Builder interface
supports Hungarian and includes a target-only translation pack, not the
private editable XLIFF or original English source texts. The user does not need to install Python, Homebrew, Xcode or anything else to build an
unsigned IPA; the only user-supplied input is the original 1.8.1g ZIP. Apple
signing credentials, Xcode and device pairing remain required only for the
optional signed IPA export or direct iPhone installation. The private app is
ad-hoc signed, not notarized or App Store distributed. macOS Gatekeeper may
request explicit approval if the app carries download quarantine.
macOS 26.6 or later is required.
This does not change the game executable's macOS 14 deployment minimum.

### One-step game installation

On Mac v1.2.7 or later, choose the original ZIP, select the connected
iPhone or iPad and press Install Game. The same local AOT assembler creates
an ephemeral unsigned IPA, then the Builder signs its app with a matching
locally available Apple development profile and installs it using devicectl.
The signing settings can be expanded if the default Team or bundle identifier
requires correction. Keep the same bundle identifier when updating a game to
preserve its on-device save container.

If Xcode has not created a development provisioning profile covering the
selected device and identifier, configure automatic signing in Xcode first.
The Builder does not distribute or create developer certificates, and never
uploads the user's original game data. The exported IPA is optional; use
Save IPA to create an unsigned file for later private signing elsewhere.
An IPA built for someone else will require their own appropriate signature
before installation.

The log area wraps long compiler lines and automatically follows vertical
progress only. Any unsuccessful signing or installation is reported as a
failure; the temporary source and signing material are cleared afterward.
Windows remains a Create IPA-only application because it does not have
Apple's local Xcode installation and device signing integration.

### Compiler logs and application status

Native AOT compiler diagnostics are logged through the same typed JSON
protocol as other Builder log lines. The normal block-discovery figures
are informational. They must not change the status indicator to an error
or stop the build. A genuine nonzero translator exit is still an error.
The complete session log is available from the Builder's detail controls.

## Portable Mac download and integrity

Download MinionRush-Builder-macOS-arm64.zip and SHA256SUMS.txt
from the same published GitHub Release. Version 1.2.4 displays the version
in the Mac application header. The downloaded ZIP contains the full portable
Builder with NativeKit, PythonRuntime and the Hungarian pack. The separate
source-only `build/installer/Minion Rush Installer.app` does not contain
NativeKit and cannot make an unsigned IPA. In Terminal run:

    shasum -a 256 MinionRush-Builder-macOS-arm64.zip

Compare the result with the Mac archive line in SHA256SUMS.txt. This
checks integrity against the published digest, not publisher identity.
Extract the .app, select your lawfully obtained original ZIP and create
the unsigned IPA. macOS may require explicit per-app approval in System
Settings > Privacy & Security > Open Anyway. Do not disable Gatekeeper
or remove download quarantine; managed Macs may prohibit approval.
Sign the resulting IPA separately before installation. Back up existing
saves before changing an app identifier or signing team.

## Build and use

The generic v3 template is created once on the developer Mac without
supplying the original game:

```bash
mkdir -p build/ios-generic
./tools/build_ios_template.sh \
  "$PWD/build/ios-generic/MinionRush-generic-template-unsigned.zip"
```

```bash
./tools/build_installer.sh
./tools/test_installer.sh
python3 tools/build_portable_mac_builder.py --app "build/installer/Minion Rush Installer.app" \
  --template "build/ios-generic/MinionRush-generic-template-unsigned.zip" \
  --python-runtime-archive "build/private-runtime-cache/cpython-3.14.8-aarch64-macos.tar.gz" \
  --output "build/private/Minion Rush IPA Builder.app"
open "build/private/Minion Rush IPA Builder.app"
```

The packaged Python runtime is a developer-side build input, never an
end-user dependency. Use the upstream [20261003 python-build-standalone
release](https://github.com/astral-sh/python-build-standalone/releases/tag/20261003)
and the macOS Apple Silicon CPython 3.14.8 install_only_stripped archive.
The packager checks its SHA-256 against
`ed14e9042f36c0aa18383f5d9729bcbf17847e842666a793b5a5a5d8505c0ca1`
before embedding the runtime. CPython's original license and pinned component
notices are included in the portable package's PythonRuntime and Runtime
license directories. The portable .app uses the embedded Python directly
and refuses to run without it, rather than silently using the user's Python.
Only the standard library modules required by the Builder are packaged:
unused Tcl/Tk, pip, IDLE, `ensurepip`, and their development files are
excluded. Their removal does not modify the verified upstream archive,
which is kept as a developer-side build input. The portable app is about
51 MB, and its unsigned IPA workflow does not create redundant cached
public source snapshots. The Xcode signing path retains source validation.

The complete portable Builder embeds a verified generic template, host AOT
compiler and Python runtime. The source-only Xcode importer does not include
the native kit. Use the portable Mac Builder for this maintained workflow.

Connect the device, unlock it, trust the Mac, and enable Developer Mode.
Choose the original ZIP, select a device and press **Install Game**.
The ZIP is validated locally and native ARM64 game code is generated once.
A matching existing Xcode provisioning profile and local development
certificate are checked *before* compilation. The locally built unsigned
IPA is then unpacked, signed and installed on the selected device. No second
ZIP import or Xcode game build is performed. On success the temporary IPA
and signed staging files are removed. The app is not uninstalled during
updates, so keep the previously used bundle identifier to retain its save
container.

**Signing settings** contains the detected local Team and the bundle
identifier. These options are normally left unchanged. If no profile
covers the selected device and identifier, use Xcode automatic signing once
to register the device and refresh the Builder. Free Personal Team profiles
normally expire after seven days and have a limit on installed apps.

**Save IPA...** creates an optional unsigned IPA at a chosen location
without signing or installing it. This is useful for a private handoff to
someone who can sign the IPA for their own device. Do not distribute the
original game ZIP or generated IPA publicly without appropriate rights.
Replacement of an existing export requires confirmation.

**Cancel** interrupts the current process; it does not uninstall or erase
existing game data. Cancellation during an active installation cannot
necessarily reverse that installation. The full activity log remains
available locally for troubleshooting. Long compiler lines wrap at the
left edge, with vertical following only.

## Data and architecture

User data stays in `~/Library/Application Support/MinionRushInstaller`:

| Directory | Content |
|---|---|
| `assets` | Imported private release assets |
| `runtimes/<source-hash>` | Public source snapshot and its generated build output |
| `DerivedData` | Signed app and Xcode products |
| `logs` | Complete, timestamped operation logs, including failures and cancellation |

**Show local files** opens this directory. Public source snapshots are keyed by
their source content and executable permissions, so an app update cannot merge
incompatible source trees. Every operation checks the cached public files against
the bundled snapshot before using them. Missing, edited, or linked source files
are rejected; the installer does not merge or repair a damaged snapshot silently.
Assets remain separate from these snapshots. A newer source version does not
migrate or build an older snapshot. Remove unwanted local files only after quitting
the installer. Removing `assets` requires importing the ZIP again. Logs are local
and are not uploaded; review them before sharing because Xcode/device output may
contain user paths, signing-team metadata, and device identifiers. Log files use
owner-only permissions and persist after the app closes.

The public app includes the maintained public source, bilingual UI contract,
native executable and generic project icon. The optional private Mac Builder
additionally packages an unsigned iOS template, a host-native ARM compiler and
the locally configured, optional user-provided catalog. The bundled
runtime and compiler are SHA-256-verified.
Neither version includes the original release ZIP, Apple credentials or
provisioning profiles; public distribution of the private kit needs separate
rights clearance.

`InstallerModel` owns UI state; `InstallerView` arranges the workflow and
`InstallerControls` supplies reusable cards, fields, and native full-width
pop-up controls. `BackendProcess` runs a bounded JSON Lines
stream off the main thread and uses argument arrays, not interpolated shell
commands. Blocking POSIX pipe reads return available bytes immediately, retry
interrupted reads, and sleep when there is no output; they do not wait for a
fixed-size buffer to fill or poll a timer. `installer_backend.py` adapts these
requests to the existing asset validator/importer and deployment tools.
A workspace lock rejects simultaneous
operations, and the deployment tools retain their shared build lock. There is
no background device polling, telemetry, or automatic download worker.
`installer_protocol.py` owns events, full logs, and workspace locking.
`ipa_archive.py` packages the signed application into a temporary IPA and
commits it atomically after writing succeeds. No separate Windows backend,
USB library, or third-party signing dependency is included in the project.

`tools/signing.py` validates the UI contract's signing rules for both the
backend and command-line scripts. Xcode can resolve its configured team when
the command-line workflow omits an explicit Team ID; the native installer
requires one. The Swift UI also requires a complete regex match, so whitespace
and trailing newlines cannot pass one layer and fail another.

## Native Mac and Windows consistency

The Mac and Windows Builders use the same host ARM64 translator, source
validation, optional Hungarian language pack, theme and release version.
Windows creates an unsigned IPA for separate signing. Mac additionally
offers direct installation to a paired iPhone or iPad, using a compatible
local Apple development identity and provisioning profile. The Mac app
also has optional Save IPA, without a mandatory output destination.
Its device Refresh button checks Xcode directly without reimporting assets.
English and Magyar switch the Builder interface independently of the
game's language resources.
The primary action is gold; selected language indicators are solid green,
with no tick. Both apps use the same icon source. The shared palette is
`config/builder_theme.json`, which generates native C and Swift color
definitions through `tools/build_builder_palette.py`. Build scripts and
CI verify that both platform definitions match this file. Cards, margin
rhythm and disabled-state behavior remain native to each OS.

The Mac UI is still native SwiftUI; its common controls and theme are in
installer/InstallerControls.swift. The existing signing/team/device options
are retained behind an optional disclosure section. Signing still uses
Xcode and Apple provisioning, not a Windows sideloading dependency.
The macOS ZIP and save dialogs retain native focus, accessibility and
overwrite-confirmation behavior. Logs can be copied, opened and exported.

Build the portable v3 Builder without private game material:

```bash
python3 tools/build_portable_mac_builder.py \
  --app "build/installer/Minion Rush Installer.app" \
  --template "build/ios-generic/MinionRush-generic-template-unsigned.zip" \
  --python-runtime-archive "build/private-runtime-cache/cpython-3.14.8-aarch64-macos.tar.gz" \
  --output "build/private/Minion Rush IPA Builder.app"
```

The portable output path must not exist before packaging. The archive is
pinned and hash-checked, and the resulting portable `.app` is ad hoc signed.

This is an ad hoc signed candidate, not a notarized public release.
The project-owned executable contains no original game data before a user
supplies the game ZIP. The unsigned native IPA is assembled using the same
prepare_native_aot.py and native_ipa.py modules as the Windows builder.
The portable bundle validates its compiler and generic template hashes and
an optional separately supplied language catalog, if present.
Its embedded Python runtime is version- and checksum-pinned, independently
ad-hoc signed and relocatable inside the signed .app.
The previous IPA is protected: a candidate is completed and verified in an
isolated destination-side directory before atomic replacement.
The original ZIP must always be supplied by the user. The 1.2.3 generic
runtime is compiled independently of that ZIP; its engine-specific map and
binding values are generated only during the user's local IPA build.

## Verification and distribution

The Python suite exercises import rollback, rejected archives, exact device
selection, process-group cancellation (including an exited leader), private
complete logs, shared signing validation, public-source packaging, and the UI
contract. `tools/test_installer.sh` exercises the actual Swift process runner,
live delivery before worker exit, fragmented UTF-8 output, cancellation,
deliberate device selection, Install gating, and native selector layout without
installing to a device. An optional snapshot-directory argument captures only
the synthetic test window for light/dark, small-window, failure, and busy review.
IPA export tests cover bundle layout, byte and executable-permission preservation,
invalid input, rejected links, atomic replacement, and cleanup after failed writes.
These checks do not validate a third-party signing tool or Windows USB drivers.

Historical Builder validation (1.1.0, 2026-10-13) includes native SwiftUI process,
control and layout tests, Mac and Windows source/format checks, and an
end-to-end private Mac IPA build and overwrite using the original ZIP. The
resulting unsigned IPA passed ZIP-integrity and Hungarian resource checks.
The Windows 11 ARM workflow was separately tested with Sideloadly and a
physical iPhone; the user confirmed installation and gameplay. Those private
tests do not certify other machines, iOS versions, signing renewal, or
macOS Gatekeeper behavior.

An earlier Xcode-based Mac installer was used to install on a physical
iPhone, but the redesigned 1.1.0 interface has not undergone a fresh complete
physical-device install test. The attempted current UI screenshot capture was
blocked by the remote macOS WindowServer. Light/dark and manual visual
inspection, clean-Mac browser download, macOS 26.6 and physical iPad testing
remain release checks.

The source repository contains no private assets or signing material. The
download package also excludes these files. Paid developer membership is not
required for this project's ad-hoc distribution workflow:

```bash
./tools/package_installer.sh
```

This builds the native app, runs its process/model tests, and creates a ZIP and
matching SHA-256 file in `build/releases`. The ZIP contains only the installer,
the [download guide](INSTALLER_DOWNLOAD.md), and the source license. The package
validator requires an exact public-file allowlist, byte-identical source,
ARM64 executable permissions, and matching archive content. The script extracts
the archive and verifies the app's signature again before producing the final
files. Generated archives and checksums are not committed to Git.

The default app is ad-hoc signed with Hardened Runtime. It is not Developer ID
signed, notarized, or reviewed by Apple. A downloaded copy may require the user
to open System Settings > Privacy & Security > Open Anyway after trying to
launch it. Publish the ZIP together with its checksum and guide. A checksum
detects changes relative to that file; it does not authenticate the publisher.
Never ask users to disable Gatekeeper, remove quarantine, or override a malware
or invalid-signature alert.

A browser-downloaded ZIP should also be tested on a clean Mac: checksum,
extraction, first-launch warning, optional user approval, and ZIP-to-IPA
creation. This is a documentation and compatibility check, not a requirement
to obtain publisher credentials. Approval may be unavailable on managed Macs.

The release build uses ad hoc signing only. The packaging script has no
publisher-signing or notarization mode. This does not affect the independent
signing required to install an IPA on an iPhone or iPad. Neither the Builder
nor this guide disables operating-system trust protections.

Apple references are listed in `TECHNICAL_SOURCES.md`. Asset ownership and the
project's legal scope are described in `LEGAL.md`.
