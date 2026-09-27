# Minion Rush Installer for Windows

The native WPF client installs a signed IPA exported by the Mac installer.
It does not build the game from the original release ZIP, sign in to Apple,
or create a provisioning profile. Xcode is not needed on Windows, but the IPA
must first be built and signed on a Mac for the target device.

The Windows x64 client passes native WPF interaction and state/process tests
on GitHub's Windows runner. The frozen helper passes its dependency/API smoke
test, and the package passes public-content, executable-architecture, license,
and extracted-archive hash checks. This is a prerelease: physical USB installation
through Apple's Windows service, a clean-system security/driver setup, and ARM64
execution remain unverified. CI does not replace those checks.

## Prepare the IPA

1. On the Mac, import your legally obtained 1.8.1g release ZIP.
2. Connect and select the iPhone or iPad that will receive the game.
3. Enter your signing details, keeping the same bundle identifier for updates.
4. Choose **Export IPA** to build and sign without installing.
5. Transfer the private IPA to Windows. Never publish it or include it in Git.

Free Personal Team profiles expire after seven days. Export again when the
profile expires or when adding a device outside that profile. Changing computers
does not extend its lifetime. The installer does not remove other apps to work
around Apple's three-app limit.

## Install on Windows

Use Windows 11 and install [iTunes from Apple](https://www.apple.com/itunes/download/win64).
It supplies the Apple Mobile Device Service. Pair through iTunes, unlock the
device, approve **Trust This Computer**, and enable Developer Mode. The game
requires iOS/iPadOS 17 or later.

Extract the entire installer ZIP; keep `backend` next to `MinionRushInstaller.exe`.
Run without administrator privileges. Click anywhere in the IPA area or drop one
`.ipa` onto it. Select the device, review the read-only signing details, and choose
**Install**. Multiple devices require an explicit selection. Launch, import, and
Refresh never install automatically.

Preflight checks archive layout, bounded metadata, guest version, profile
expiration, application identifier, device membership, and minimum OS. CMS
decoding is not certificate-chain or executable-signature verification. The
device makes the final Apple signature/provisioning decision. Installation is
checked in its app database; startup and gameplay require a device check.

The log streams upload and installation progress. **Follow output** can be
disabled, and **Open full log** opens the complete transcript. Logs stay under
`%LOCALAPPDATA%\MinionRushInstaller\logs`, inheriting user-profile Windows access
controls. Review before sharing: paths, device IDs, and team metadata can appear.

**Cancel** requests cleanup of only this operation's staging file when reachable.
A started device installation may still finish. Cancellation never uninstalls
the game or deletes saves. Quitting while busy requires confirmation.

## Build from source

On a Windows machine of the target architecture, install PowerShell 7, .NET 10
SDK, and Python 3.13 or later, then run:

```powershell
./tools/build_installer_windows.ps1
```

`win-x64` is the default. `-Runtime win-arm64` requires Windows ARM64 and ARM64
Python so the frozen helper matches the target. The builder runs IPA and .NET
state/process tests, publishes a self-contained native GUI, freezes its helper,
and packages public code, dependencies, licenses, and this guide. No game assets,
IPA, profile, Apple credentials, or SDK are included. Outputs and checksums stay
under `build/windows`, ignored by Git.

The executable is unsigned. SmartScreen can warn about an unknown publisher;
Smart App Control or organization policy can block it entirely. There is not
always a per-app **Run anyway** option. Do not disable security protections or
bypass malware/invalid-signature alerts. Stable release verification still needs
first launch, USB driver setup, and installation on a clean Windows system.

Download the matching `.sha256` file beside the ZIP. To compare its recorded
SHA-256 with the actual archive in PowerShell:

```powershell
$Archive = "minion-rush-installer-1.0.0-win-x64.zip"
$Expected = (Get-Content "$Archive.sha256").Split()[0]
if ((Get-FileHash $Archive -Algorithm SHA256).Hash -ne $Expected) {
    throw "Download checksum mismatch."
}
```

A checksum detects changed download bytes; it is not a publisher certificate.

References are listed in [TECHNICAL_SOURCES.md](TECHNICAL_SOURCES.md).
