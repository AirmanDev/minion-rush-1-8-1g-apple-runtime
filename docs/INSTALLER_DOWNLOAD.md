# Minion Rush Installer 1.1.0 RC1 (macOS)

This public download is the limited Xcode-based Mac installer, not the
full portable Builder. The full Mac and Windows Builder binaries are not
public release assets pending separate distribution-rights review.
This is an asset-free Mac installer. It imports a lawfully
obtained original Minion Rush 1.8.1g ZIP, signs the iOS app using the owner's
Xcode setup, and installs it on a connected device or exports a signed IPA.
It does not contain the original game, an Apple account, signing credentials,
or the private prebuilt iOS template.

## Requirements

- Apple Silicon Mac running macOS 26.6 or later
- Xcode 27 with iOS platform support and an Apple Account configured for signing
- Paired iPhone or iPad running iOS/iPadOS 17 or later for installation
- User-supplied, unmodified original 1.8.1g release ZIP

The private, standalone Mac and Windows IPA Builders have a separate,
Xcode-free unsigned-IPA workflow. Those complete binaries are **not**
distributed in this public release because their template and optional
Hungarian translation have unresolved redistribution questions. The public
Mac app therefore displays the unsigned Create IPA action as unavailable.
Its Xcode-based import, signed export, and device installation remain usable.

## Download

Get the following files from the official
[GitHub release](https://github.com/AirmanDev/minion-rush-1-8-1g-apple-runtime/releases):

- `minion-rush-installer-1.1.0-macos-arm64.zip`
- `minion-rush-installer-1.1.0-macos-arm64.zip.sha256`

Keep both files in the same folder. Verify the downloaded ZIP in Terminal:

```bash
shasum -a 256 -c minion-rush-installer-1.1.0-macos-arm64.zip.sha256
```

The checksum detects file changes, but does not authenticate the publisher.
Extract the ZIP and move `Minion Rush Installer.app` to Applications.

The application is ad hoc signed, not Developer ID signed or notarized.
macOS may block the first launch. If you trust the verified download, try
opening it and then use System Settings > Privacy & Security > Open Anyway.
Do not disable Gatekeeper, remove quarantine, or override a malware warning
or invalid-signature warning. Apple's instructions:
https://support.apple.com/guide/mac-help/open-a-mac-app-from-an-unknown-developer-mh40616/mac

## Use

1. Open Xcode once, sign in to your Apple Account and finish initial setup.
2. Connect and unlock the device, trust the Mac and enable Developer Mode.
3. Open the app, select your original ZIP, and expand
   **Sign and install with Xcode**.
4. Choose **Import ZIP for signed installation**, then select the device,
   signing team, and bundle identifier.
5. Choose **Install** or **Export signed IPA**. Keep the same team and bundle
   identifier when updating an existing installation.

A new Personal Team may need a one-time test project launched from Xcode to
create a development profile. Free-account signatures typically expire after
seven days. Existing save data may depend on the bundle identifier and
signing identity.

The app and original game content are independent third-party works. This
project is not endorsed by Gameloft. Never publish a completed IPA, private
game archive, translation catalog or credentials. For detailed setup and
limitations, see [Mac installer documentation](https://github.com/AirmanDev/minion-rush-1-8-1g-apple-runtime/blob/main/docs/INSTALLER.md)
and [legal scope](https://github.com/AirmanDev/minion-rush-1-8-1g-apple-runtime/blob/main/docs/LEGAL.md).
