# Native installer

The installer is a SwiftUI macOS application for importing the original
Minion Rush 1.8.1g release and installing this runtime on one selected iPhone
or iPad. It is not the game, an asset downloader, or a signing service.

## Requirements

- Apple Silicon, macOS 26.6 or later; macOS 27 is included in the target range
- Xcode 27 or later, with iOS platform support and first-launch setup completed
- An Apple Account signed in to Xcode
- A paired iPhone or iPad running iOS/iPadOS 17 or later
- A legally obtained, unmodified 1.8.1g release ZIP containing its APK and data
- Space for extraction, validated assets, compiled code, and Xcode build products

The installer uses Xcode's Python 3; Homebrew is not required. The complete
installation workflow needs Xcode, not just Command Line Tools. Xcode 27
requires macOS 26.6, so the installer does not advertise macOS 14 compatibility.
This does not change the game executable's macOS 14 deployment minimum.

## Build and use

```bash
./tools/build_installer.sh
./tools/test_installer.sh
open "build/installer/Minion Rush Installer.app"
```

The installer itself builds without private assets. Drop one release ZIP onto
the archive area, or use **Choose ZIP**. Existing assets are replaced only after
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
user to unlock or approve something on the device. The activity log opens when
an operation starts and streams ZIP extraction, APK verification, asset copying
and validation, build output, signing, installation, and startup checks. Each
operation also saves a complete private log; **Open full log** opens the current
file. **Follow output** can be disabled to inspect earlier lines. The UI keeps
a bounded recent tail rather than rendering an unlimited build transcript.
No device is installed automatically on app launch or archive import.

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
their content hash, so an app update cannot merge incompatible source trees.
Assets remain separate from these snapshots. A newer source version does not
migrate or build an older snapshot. Remove unwanted local files only after quitting
the installer. Removing `assets` requires importing the ZIP again. Logs are local
and are not uploaded; review them before sharing because Xcode/device output may
contain user paths, signing-team metadata, and device identifiers. Log files use
owner-only permissions and persist after the app closes.

The app bundle contains only the maintained public source manifest, UI
contract, native executable, and an original generic installer icon. It does
not package the engine, translated engine blocks, game artwork, audio, release
ZIP, private XLIFF files, provisioning profiles, or credentials. Optional
community language packs therefore remain a separate local setup described in
`LOCALIZATION.md`; importing a release enables its original languages.

`InstallerModel` owns UI state. `BackendProcess` runs a bounded JSON Lines
stream off the main thread and uses argument arrays, not interpolated shell
commands. Blocking POSIX pipe reads return available bytes immediately, retry
interrupted reads, and sleep when there is no output; they do not wait for a
fixed-size buffer to fill or poll a timer. `installer_backend.py` adapts these
requests to the existing asset validator/importer and deployment tools.
A workspace lock rejects simultaneous
operations, and the deployment tools retain their shared build lock. There is
no background device polling, telemetry, or automatic download worker.

`tools/signing.py` validates the UI contract's signing rules for both the
backend and command-line scripts. Xcode can resolve its configured team when
the command-line workflow omits an explicit Team ID; the native installer
requires one. The Swift UI also requires a complete regex match, so whitespace
and trailing newlines cannot pass one layer and fail another.

## UI contract and Windows

`config/installer_ui.json` is the shared source for English labels, section
order, minimum width, and signing-input rules. The stable arrangement is:

1. Header: title, then release/platform subtitle.
2. Release: drop target and chooser, validation state, then ownership notice.
3. Device: picker on the left, Refresh on the right, then connection guidance.
4. Signing: detected team picker when available, Team ID, bundle identifier,
   guidance, then Xcode/account-help actions.
5. Collapsible activity log.
6. Persistent footer: status on the left; Cancel and primary Install on the right.
7. Window toolbar: Show local files.

The future Windows client should consume the same contract and preserve this
order and placement for equivalent states, including the full-log action and
follow-output checkbox, using native Windows controls and accessibility.
Platform fonts, control metrics, and material appearance are not
pixel-identical. The Mac uses standard SwiftUI forms, controls, and a native
Liquid Glass primary button; it has no web view or imitation glass renderer.

The current build/deployment adapter is macOS-specific. Xcode does not run on
Windows. A Windows client therefore still needs a separately designed signing
and device-installation backend, or an explicit Mac build companion. A shared
UI contract does not by itself make Windows installation supported.

## Verification and distribution

The Python suite exercises import rollback, rejected archives, exact device
selection, process-group cancellation (including an exited leader), private
complete logs, shared signing validation, public-source packaging, and the UI
contract. `tools/test_installer.sh` exercises the actual Swift process runner,
live delivery before worker exit, fragmented UTF-8 output, cancellation, and
Install gating without installing to a device. Public-source CI runs the Python
suite; native tests require the macOS/Xcode environment above.

The installer was built and visually checked on macOS 27 with Xcode 27. The
original ZIP passed import through both the backend and the native file chooser.
The connected iPhone and iPad were enumerated. The user completed a separate-app
iPhone installation through the native GUI on iOS 27. Its persisted operation
log confirms compilation, signing, packaging, installation, and startup
verification; the user also confirmed that the live activity log worked.
The three-app free-signing limit was reproduced before that successful attempt.
Runtime testing on macOS 26.6 and physical iPad installation through this GUI
remain release checks.

The source repository is independently publishable: it contains no private
assets or signing material. A downloadable installer binary has additional
distribution requirements; a successful local installation is not a
notarization or clean-Mac distribution test.

The default build is ad-hoc signed for local use, not notarized for public
downloads. For distribution, build with your Developer ID Application identity:

```bash
./tools/build_installer.sh --identity "Developer ID Application: Your Name (TEAMID)"
ditto -c -k --keepParent "build/installer/Minion Rush Installer.app" /tmp/installer.zip
xcrun notarytool submit /tmp/installer.zip --keychain-profile YOUR_PROFILE --wait
xcrun stapler staple "build/installer/Minion Rush Installer.app"
```

Create the final download archive after stapling. Use your own credentials,
verify the notary result and Gatekeeper assessment, and test the downloaded app
on a clean Mac before publishing. The build enables Hardened Runtime and adds
a secure timestamp when a distribution identity is provided. It does not
disable Gatekeeper or the device's signing/trust protections.

Apple references are listed in `TECHNICAL_SOURCES.md`. Asset ownership and the
project's legal scope are described in `LEGAL.md`.
