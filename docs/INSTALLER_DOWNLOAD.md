# Minion Rush Installer for macOS

This installer imports your own original Minion Rush 1.8.1g release ZIP and
builds the preservation runtime for one connected iPhone or iPad. No game
assets, Apple credentials, or signing profiles are included in this download.

## Requirements

- An Apple Silicon Mac running macOS 26.6 or later
- Xcode 27 or later, including iOS platform support and first-launch setup
- Your Apple Account signed in to Xcode; paid membership is not required
- A paired iPhone or iPad running iOS/iPadOS 17 or later
- Your own lawfully obtained, unmodified Minion Rush 1.8.1g release ZIP

## Download and open

Download the ZIP and its matching `.sha256` file from the official repository:
https://github.com/AirmanDev/minion-rush-1-8-1g-apple-runtime

The checksum detects a damaged or changed download. It is not an Apple
certificate or independent proof of the publisher's identity. In Terminal,
open the folder containing both files and verify them with:

```bash
shasum -a 256 -c minion-rush-installer-1.0.0-macos-arm64.zip.sha256
```

1. Extract the ZIP and read this guide.
2. Move `Minion Rush Installer.app` to Applications.
3. Try opening the app. This build is ad-hoc signed, not Developer ID signed
   or notarized. macOS can block it because Apple has not verified it.
4. If you trust this download, open System Settings > Privacy & Security.
   Find the installer under Security and choose Open Anyway. Confirm the
   prompt and authenticate if macOS asks.
5. macOS remembers the exception for this app. A different download or update
   may require another approval.

The approval button is available for about an hour after an opening attempt.
Managed Macs may prevent exceptions; ask the administrator rather than
changing system-wide security settings. Do not override a malware alert or
an invalid/damaged-signature warning. Re-download and check the package, or
build the published source yourself. This project does not ask you to disable
Gatekeeper, remove download quarantine, or install an untrusted certificate.

Apple's instructions:
https://support.apple.com/guide/mac-help/open-a-mac-app-from-an-unknown-developer-mh40616/mac

## Install the game

Open Xcode once and sign in to your Apple Account. Connect and unlock the
device, trust this Mac, and enable Developer Mode. Drop the release ZIP into
the installer, choose the device, select or enter your signing team, and
press Install. For a new Personal Team without a cached profile, first run
a small iOS app from Xcode on the device with that team selected.

The activity log shows extraction, validation, compilation, signing, device
installation, and startup verification. Open full log opens the complete
private transcript. Logs can contain personal paths and device/team metadata;
review them before sharing.

Keep the same bundle identifier and signing team to update an existing game
without deleting its data. A different identifier installs a separate app.
Free Personal Teams allow three installed apps per device, and their signing
profiles expire after seven days. Reinstall with the same identifier to renew
the profile. The installer does not remove another app automatically.

The repository's Apache License 2.0 covers the original project code, not
Minion Rush, its trademarks, engine, characters, artwork, or other game data.
This is an independent preservation project, not an official Gameloft release.
