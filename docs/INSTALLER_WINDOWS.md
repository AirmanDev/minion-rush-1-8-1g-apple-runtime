# Windows IPA Builder

## Download and supported systems

The initial public release of **Unofficial IPA Builder v1.2.8** is
available on [GitHub Releases](https://github.com/AirmanDev/minion-rush-1-8-1g-apple-runtime/releases/tag/builder-v1.2.8).

Download `MinionRush-Builder-Windows-x64.exe`, the complete portable
Windows 11 x64 Builder. It also runs on Windows 11 ARM using x64
emulation. The EXE embeds the required Python runtime, native ARM64
translator and generic unsigned iOS template. There is no system-wide
installer, external Python requirement or bundled original game.

Check the SHA-256 digest before running the application. The executable
is not Authenticode signed and may trigger a Windows SmartScreen
unknown-publisher warning. Approve an individual downloaded file only
after verifying its source and checksum. Never disable Windows
Defender or bypass an actual malware warning.

## Create IPA

1. Supply your own lawfully obtained, unmodified original Minion Rush
   1.8.1g ZIP.
2. Open the portable Builder.
3. Select the ZIP and the destination for the unsigned IPA.
4. Leave **Include additional languages** enabled if you want Magyar
   added as a separate in-game language.
5. Click **Create IPA**. Wait for translation and archive verification.

The host translates the original ARM32/Thumb engine into native ARM64
code ahead of time and assembles the IPA from the user's original data.
No private game ZIP, precompiled game-specific native code, generated IPA
or Apple credentials are included in the public Builder.

The native iOS template and compiler match the Mac edition. The
Hungarian target-only text pack is compiled locally after checking
the original text-table fingerprint; it does not replace existing
game languages or perform runtime translation.

## Signing and installation

A Windows-created IPA is **unsigned**. Sign and install it with a
compatible external tool such as
[Sideloadly](https://sideloadly.io/). The tool is not bundled or
maintained by this repository. Use your own Apple account and device.
Free Apple-account profiles often expire after seven days.

A private Windows 11 ARM/x64-emulation test generated an IPA and
installed it on an iPhone with Sideloadly, confirming working gameplay,
audio, Hungarian text and save continuity. This does not guarantee
every device and operating-system version.

The Mac edition additionally supports direct Install Game with a local
Xcode signing identity and a selected physical device. That feature is
not available in Windows because Xcode's local signing environment
and devicectl are macOS tools.

## Diagnostics and source builds

The Builder shows progress and logs for native AOT compilation and
archive creation. The full log can be saved locally. A failed build
does not overwrite a previously completed IPA without explicit
replacement handling. The Windows portable kit unpacks into a
temporary location on launch and cleans up after exit.

Project maintainers can build the Win32 GUI, kit and single executable
using `tools/build_windows_gui.sh`, `tools/build_windows_kit.py`
and `tools/build_windows_onefile.py` on a configured build host.
Private UTM transport fixtures and original game assets must not be
included in public packages. Run the full source and packaging
validators and verify the final PE self-test.

The project is independent of Gameloft and the game's other
rightsholders. Original-game use and locally generated IPAs remain
subject to copyright and contract rules. The Hungarian target-only
translation may carry separate rights obligations.

See [Mac Builder](INSTALLER.md), [release scope](RELEASE_SCOPE.md),
[additional languages](LOCALIZATION.md) and [legal context](LEGAL.md).
