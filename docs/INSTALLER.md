# Native installer

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
supports Hungarian; the private Hungarian game-text catalog is not bundled. The user
does not need to install Python, Homebrew, Xcode or anything else to build an
unsigned IPA; the only user-supplied input is the original 1.8.1g ZIP. Apple
signing credentials, Xcode and device pairing remain required only for the
optional signed IPA export or direct iPhone installation. The private app is
ad-hoc signed, not notarized or App Store distributed. macOS Gatekeeper may
request explicit approval if the app carries download quarantine.
macOS 26.6 or later is required.
This does not change the game executable's macOS 14 deployment minimum.

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
before embedding the runtime. CPython and dependency licenses are retained
in the embedded library. The portable .app uses the embedded Python directly
and refuses to run without it, rather than silently using the user's Python.
Only the standard library modules required by the Builder are packaged:
unused Tcl/Tk, pip, IDLE, `ensurepip`, and their development files are
excluded. Their removal does not modify the verified upstream archive,
which is kept as a developer-side build input. The portable app is about
51 MB, and its unsigned IPA workflow does not create redundant cached
public source snapshots. The Xcode signing path retains source validation.

The separate public Xcode-only app is built without game assets or a native iOS template.
Its **Create IPA** action is therefore disabled; it supports the existing
Xcode-based import, signed IPA export, and device installation instead.
The separately built portable app includes a verified game-independent iOS
template, the native ARM64 translator, and an embedded Python runtime.
No translated game text or precomputed engine-derived metadata is bundled.
It does not embed the original game ZIP. In the private app, select your ZIP
and use **Create IPA** for unsigned output. For Xcode signing, choose
**Import ZIP for signed installation**. Existing assets are replaced only after
the entire candidate passes the shared engine, icon, game-data, and integrity
checks. Rejected imports and cancellation before the commit leave the previous
assets intact. Once committed, the new validated asset tree remains installed.

Connect the device, unlock it, trust the Mac, and enable Developer Mode. Choose
the device and enter its signing details. The installer discovers Team IDs
from local Xcode provisioning profiles when available; these are suggestions,
not proof that a certificate or account is currently usable. The user can
always enter a Team ID manually. The installer does not collect Apple Account
passwords or export private signing keys.

For a new Personal Team with no cached profile, run a small iOS app once from
Xcode on your device, selecting your team in Signing & Capabilities. Xcode
creates the development profile. Return to the installer and choose Refresh
to discover that Team ID; no manual project-file editing is required.

Use your own bundle identifier for a new installation. Keep the same signing
team and bundle identifier when updating an existing game. The installer never
uninstalls the existing app; changing its identifier creates a separate app
with a separate data container. A free Personal Team allows at most three
installed apps per device. Its profiles expire after seven days. If three
free-signed apps are already installed, remove one yourself before adding a
separate app, or update a matching existing app instead. The installer does
not remove apps or change their identifiers automatically.

**Install** becomes available after import, device selection, toolchain checks,
and signing-input validation. It builds, signs, installs, and verifies startup
through `tools/deploy_ios.sh`. Apple may still reject provisioning, or ask the
user to unlock or approve something on the device. The activity log opens for
import, export, installation, and failures, not for a successful routine refresh.
It streams ZIP extraction, APK verification, asset copying
and validation, build output, signing, installation, and startup checks. Each
operation also saves a complete private log; **Open full log** opens the current
file. **Follow output** can be disabled to inspect earlier lines. The UI keeps
a bounded recent tail rather than rendering an unlimited build transcript.
No device is installed automatically on app launch or archive import.

**Export IPA** uses the same build, signing, and packaging steps, but stops before
device installation. Choose a save location in the native save dialog. The selected
device must already be connected so Xcode can register it in the profile. The
resulting IPA includes private assets and device-bound signing material; never
publish it or commit it to Git. Transfer it privately for installation with
[Sideloadly on Windows](INSTALLER_WINDOWS.md). Sideloadly's Apple ID mode can
re-sign the IPA for another device or renew an expired profile; installation
without re-signing still requires a valid profile for that exact device.
Failed archive writes preserve an existing IPA. The destination cannot be inside
the signed app bundle, because creating an archive there would modify that bundle.

**Cancel** stops the worker and its deployment process group. A device install
is not reversible: cancellation during that phase may leave an installed app.
It never deletes the existing app or its data. Quitting during an operation
requires confirmation and waits for cancellation cleanup.

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

The portable Mac Builder offers the same primary workflow as Windows:
choose the original ZIP and unsigned IPA destination, create the IPA and
inspect the detailed log. The optional game-language toggle is disabled
unless a separately authorized translation catalog has been supplied.
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
The original ZIP must always be supplied by the user. The 1.2.2 generic
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

Before publishing a binary release, test the browser-downloaded ZIP on a clean
Mac: checksum, extraction, first-launch warning, manual approval, import, and
installation. Local build and archive verification do not cover that Gatekeeper
workflow. A clean-Mac download test remains a release check.

Developer ID signing and notarization are optional for a contributor with paid
membership who wants Apple's standard verified distribution path. The builder
accepts `--identity "Developer ID Application: Your Name (TEAMID)"` and adds a
secure timestamp for that identity. Such a release needs notarization, stapling,
and a fresh final archive; the ad-hoc packaging command does not perform those
steps. Neither workflow changes macOS or device signing/trust protections.

Apple references are listed in `TECHNICAL_SOURCES.md`. Asset ownership and the
project's legal scope are described in `LEGAL.md`.
