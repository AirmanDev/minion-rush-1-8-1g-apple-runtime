# Native installer

The Mac installer is a native SwiftUI application for creating an unsigned
IPA from the user-owned original 1.8.1g ZIP. Its optional Xcode workflow
continues to sign and install on connected Apple devices. It is not the game,
an asset downloader, or a signing service.

## Requirements

- Apple Silicon, macOS 26.6 or later; macOS 27 is included in the target range
- For unsigned IPA creation, the private Mac Builder includes a verified AOT
  translator and prebuilt unsigned iOS template; Xcode is not required at runtime.
- For the optional signed export/install, Xcode 27, an Apple Account signed
  into Xcode and a paired iPhone or iPad remain required.
- A legally obtained, unmodified 1.8.1g release ZIP containing its APK and data
- Space for extraction, validated assets, compiled code, and Xcode build products

The Python backend currently uses an installed macOS Python 3 interpreter
(from the selected Xcode toolchain or /usr/bin/python3). The private app
bundles its own ARM translator, template and optional Hungarian catalog, but
not a complete Python distribution. Therefore the .app can be moved between
compatible Macs, but a working Python 3 is still a prerequisite. The
unsigned build needs no Apple signing credentials. Only signed export and
on-device installation need the full Xcode toolchain. Xcode 27
requires macOS 26.6, so the installer does not advertise macOS 14 compatibility.
This does not change the game executable's macOS 14 deployment minimum.

## Build and use

```bash
./tools/build_installer.sh
./tools/test_installer.sh
python3 tools/build_private_mac_builder.py --app "build/installer/Minion Rush Installer.app" \
  --template "build/native-template/MinionRush-native-template-language-toggle.zip" \
  --hungarian-xlf "localizations/hu.xlf" \
  --output "build/private/Minion Rush Installer.app"
open "build/private/Minion Rush Installer.app"
```

The public app is built without private assets; the private package adds the
prepared unsigned template and locally supplied language data. It does not
embed the original release ZIP. Select a game ZIP and Create IPA to use the
new unsigned workflow. For the optional signed install workflow, choose
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
the owner-provided Hungarian catalog. The private resources are SHA-256-verified.
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

The private Mac Builder offers the same primary workflow as Windows: choose
the original ZIP, select the IPA destination, toggle the included Hungarian
language, create an unsigned IPA and inspect the detailed log. English and
Magyar switch the builder interface separately from the IPA's languages.
The primary action is gold; selected language indicators are solid green,
with no tick. Both apps use the same icon source. Navy, card backgrounds,
corner radii, margin rhythm and disabled states follow the same design.

The Mac UI is still native SwiftUI; its common controls and theme are in
installer/InstallerControls.swift. The existing signing/team/device options
are retained behind an optional disclosure section. Signing still uses
Xcode and Apple provisioning, not a Windows sideloading dependency.
The macOS ZIP and save dialogs retain native focus, accessibility and
overwrite-confirmation behavior. Logs can be copied, opened and exported.

The private Mac .app is built after the public app:
  python3 tools/build_private_mac_builder.py --app
  "build/installer/Minion Rush Installer.app" --template
  "build/native-template/MinionRush-native-template-language-toggle.zip"
  --hungarian-xlf "localizations/hu.xlf" --output
  "build/private/Minion Rush Installer.app"

This is a private, ad-hoc signed artifact, not a notarized public release.
The project-owned executable contains no original game data before a user
supplies the game ZIP. The unsigned native IPA is assembled using the same
prepare_native_aot.py and native_ipa.py modules as the Windows builder.
The private bundle validates its compiler, template and catalog hashes.
The previous IPA is protected: a candidate is completed and verified in an
isolated destination-side directory before atomic replacement.
The original ZIP must always be supplied by the user.

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

The current UI revision was built and visually checked on macOS 27 with Xcode 27,
including light/dark appearance, a minimum-size window, failure, and busy states.
Native selector tests verify full-row hit targets, binding updates, and disabled
state. Export of an existing signed app passed complete
ZIP integrity checking without installing; its temporary IPA was removed.
The earlier GUI flow's original ZIP passed import through both the backend and the native file chooser.
The connected iPhone and iPad were enumerated. The user completed a separate-app
iPhone installation through the native GUI on iOS 27. Its persisted operation
log confirms compilation, signing, packaging, installation, and startup
verification; the user also confirmed that the live activity log worked.
The three-app free-signing limit was reproduced before that successful attempt.
Runtime testing on macOS 26.6 and physical iPad installation through this GUI
remain release checks. The new UI and export changes have not been retested with
a fresh physical installation. Separately, the Windows 11 ARM workflow passed
a user-reported physical iPhone signing, USB installation and gameplay test
through Sideloadly on 2026-10-12; signing refresh is not yet verified.

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
