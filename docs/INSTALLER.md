# Mac Builder and device installation

## Download

Download the following files from the
[Unofficial IPA Builder v1.2.8 GitHub Release](https://github.com/AirmanDev/minion-rush-1-8-1g-apple-runtime/releases/tag/builder-v1.2.8):

- `MinionRush-Builder-macOS-arm64.zip`
- `SHA256SUMS.txt`

The complete portable app runs on Apple Silicon and requires macOS 26.6
or later. It includes its own Python runtime, host ARM64 translator,
generic native iOS template, project notices and optional Hungarian
game-language pack. The original game is not bundled.

Verify the SHA-256 digest, extract the ZIP and open the `.app`.
The Builder is ad hoc signed rather than Developer ID signed or notarized.
macOS may require approving the downloaded app in System Settings,
Privacy & Security. Do not disable Gatekeeper or ignore invalid-signature
or malware alerts.

## Install Game

1. Supply your own lawfully obtained, unmodified Minion Rush 1.8.1g ZIP.
2. Launch Xcode at least once and sign into your Apple Account.
3. Connect your iPhone or iPad, unlock it, trust this Mac, and enable
   Developer Mode. The device must be paired and visible in Xcode.
4. Open the Builder and select the original ZIP.
5. Select the target device and leave **Include additional languages**
   enabled if you want Magyar as an extra in-game language.
6. Check **Signing settings** if the default signing Team or bundle
   identifier needs adjustment. A matching local iOS provisioning profile
   must cover your certificate, Team, bundle ID and selected device.
7. Press **Install Game**.

The Builder runs local native ARM32/Thumb to ARM64 translation once,
generates an ephemeral unsigned IPA, signs the resulting application
using the selected local Apple development identity and installs it
through Xcode's `devicectl`. There is no duplicate ZIP import, mandatory
IPA output path or separate installation button.

The temporary IPA and signing staging files are removed after the
operation, including errors. The Builder does not uninstall the previous
game. Keep the same Team and bundle identifier for later updates so the
existing application data container and saved games can be retained.

If an appropriate provisioning profile is missing, enable Xcode automatic
signing for the device and bundle identifier first. The Builder checks
signing prerequisites before beginning the longer native build.
Free Personal Team profiles generally need renewal after seven days.

## Save IPA

Use **Save IPA...** to choose a destination and generate an unsigned
IPA without installing it. This is optional and useful for a later
private signing or installation process. A recipient must supply their
own appropriate signature and has responsibility for the legality
of their use. The Builder does not provide Apple credentials.

The input ZIP may contain only the original APK plus its complete game
data directory; unrelated APKs are not needed. The official APK is
recognized by the game-engine and icon checksums, regardless of its
filename. A Mac v1.2.8 regression verified that reduced input and the
generated unsigned IPA. See [release scope](RELEASE_SCOPE.md#reduced-zip-mac-regression-2026-10-14).

## Controls and diagnostics

**Refresh** updates Xcode's paired physical-device list without
reimporting game assets. **Cancel** interrupts the current operation;
it does not uninstall the game or erase saves. Cancellation during an
in-progress external install cannot guarantee rollback.

The Build details view keeps the title and Follow output toggle in a
separate header above two rows of equal-width log buttons. Long
compiler diagnostics wrap at the left margin, and automatic following
scrolls vertically. **Open full log** provides the local session record.

The desktop English/Magyar interface setting is independent of the
in-game language preference. When enabled, the additional Hungarian
translation is compiled as a separate game text table before the IPA
is installed. The original game languages remain available.

## Build from source

The native source-only Xcode importer in `installer/` does not contain
the complete IPA creation kit. For development, use the project's
`tools/build_installer.sh`, `tools/test_installer.sh` and
`tools/build_portable_mac_builder.py` with the generic template and
verified Python runtime archives. The downloadable Mac Builder is
the complete application; no separate source-only importer is
distributed on the public Releases page.

Before publishing a rebuilt portable app, verify its source manifest,
bundled Python and toolchain notices, app signature, SHA-256 checksum,
and one private original-ZIP-to-IPA regression. Do not commit or
upload original game assets, private XLIFF, generated game IPAs,
certificates or signing profiles.

See [release scope](RELEASE_SCOPE.md), [additional languages](LOCALIZATION.md),
[legal context](LEGAL.md) and [Windows instructions](INSTALLER_WINDOWS.md).
