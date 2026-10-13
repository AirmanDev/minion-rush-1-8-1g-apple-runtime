# Mac Xcode Installer - downloads and local builds

The published 1.1.0 RC1 is the separate asset-free, Xcode-based Mac
installer, NOT the standalone Mac/Windows native IPA Builder. Its unsigned
Create IPA action is unavailable without a portable runtime kit. It can
import the user's original ZIP and use local Xcode signing to export a signed
IPA or install to a connected iPhone/iPad.

The new 1.2.0 source tree can compile this same separate Xcode-based Mac
installer; full portable Mac/Windows Builder candidates are packaged via
`tools/build_portable_mac_builder.py` and `tools/build_windows_onefile.py`,
not this download. The 1.2.0 portable candidate has not yet passed every
legal or physical-device release gate.

## Requirements

- Apple Silicon with macOS 26.6 or later
- Xcode 27 or later, iOS SDK and an Apple Account configured for signing
- Unmodified original Minion Rush 1.8.1g ZIP supplied by the user
- Paired iPhone/iPad running iOS/iPadOS 17 or later for installation

## Existing public download

The current public Mac-only Xcode Installer 1.1.0 RC1 is available from
[GitHub Releases](https://github.com/AirmanDev/minion-rush-1-8-1g-apple-runtime/releases/tag/installer-v1.1.0-rc.1).
Download its ZIP and the matching `.zip.sha256` checksum attachment.
Keep both files together and verify the ZIP in Terminal:

```bash
shasum -a 256 -c minion-rush-installer-1.1.0-macos-arm64.zip.sha256
```

This verifies file integrity but does not authenticate the publisher.

## Build this installer locally

With the supported Apple toolchain:

```bash
./tools/package_installer.sh
```

The output is under `build/releases/`, named for the current source
version, together with its matching `.zip.sha256` file. Verify that
checksum before unpacking the downloaded archive.

The Xcode-only installer is ad hoc signed, not Developer ID signed or
notarized. macOS may block the initial launch of a trusted downloaded
copy. Use System Settings > Privacy & Security > Open Anyway after
confirming the expected download and checksum. Do not disable Gatekeeper,
remove quarantine or disregard a malware/invalid-signature warning.
See Apple's official instructions:
https://support.apple.com/guide/mac-help/open-a-mac-app-from-an-unknown-developer-mh40616/mac

## Sign or install from the Xcode workflow

1. Launch Xcode once, sign into an Apple Account and complete first-run setup.
2. Connect/unlock the device, trust the Mac and enable Developer Mode.
3. Open the installer; choose the original ZIP and expand the
   **Sign and install with Xcode** section.
4. Import ZIP for signed installation and choose the device, signing team
   and bundle identifier.
5. Choose Install or Export signed IPA. Retain the same team/identifier
   when updating an existing installation, as save data can depend on both.

A free-account signing profile may expire after seven days. This program
does not distribute original game assets, translated game strings, signed
IPAs or Apple account information.

For the complete portable Builder workflow and limitations see
[INSTALLER.md](INSTALLER.md), [NATIVE_WINDOWS.md](NATIVE_WINDOWS.md),
and [RELEASE_SCOPE.md](RELEASE_SCOPE.md).
