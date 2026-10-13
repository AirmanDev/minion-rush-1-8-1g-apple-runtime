# Windows IPA installation with Sideloadly

Two separate IPA preparation paths are supported in the source tree:

- **Mac (maintained):** The native installer exports a signed IPA using Xcode
  and the owner's Apple signing account. See [INSTALLER.md](INSTALLER.md).
- **Windows (experimental, not publicly released):** A portable builder takes
  the user's own 1.8.1g ZIP, compiles the original engine into native ARM64
  blocks on Windows, inserts them into a prebuilt unsigned iOS template, and
  packages the original assets. A private Hungarian hu.xlf catalog is needed
  for Hungarian; the original launcher icon comes directly from the ZIP. No Mac is required *for this local build*.
  See [NATIVE_WINDOWS.md](NATIVE_WINDOWS.md). Windows output is an **unsigned
  IPA** and does not install or sign itself.

To use the Mac path, export the IPA locally and transfer it privately.
The portable Windows executable opens the native Builder. Drag an original
ZIP onto the window or use Browse, choose an output IPA path with Save As,
and select Create IPA. When an IPA already exists, the Windows Save dialog
asks whether to replace it. Accepting this authorizes that file for one
build only; if a path was selected earlier, the Builder asks before replacing
it. The new IPA is assembled in a private folder on the same volume and
replaces the old file only after successful verification. Build failures do
not destroy the previous IPA.

The bundled Magyar option is a checkbox, enabled by default when its
private catalog is present. Uncheck it to exclude Hungarian resources from
that IPA; the original game's languages are always included. There is no
separate language-file picker. The game lists only installed language packs
on its initial and Settings language pages, and no XLIFF needs to be copied
to the iPhone. Use English / Magyar in the Builder header to change the
Windows interface language without changing the IPA language selection.
The window shows the current step in a compact status panel. **Show details** expands build diagnostics,
including paths, phase, transfer method and detailed error; it opens
automatically on failure. Copy error, Copy full log and Save log provide
local reporting. On failure the private workspace is retained, can be opened
through Open temp folder and must be deleted manually afterwards. Do not
share logs or temporary game files publicly. The GUI does not overwrite
without confirmation and never signs automatically. Tab and Shift+Tab move between
fields and buttons, Enter presses the focused button, and the window scales
with the display's DPI setting.

The Windows Builder GUI and portable EXE share the same original
blue-and-gold icon at 16, 24, 32, 48, 64, 128 and 256 px. Both executables
contain a Windows icon resource, so Explorer, the taskbar and the window
title bar use the application icon without sidecar assets. The icon is
generated from project-owned artwork, not original game images.

Completed-build diagnostics now refresh before the success dialog opens;
the diagnostic status therefore shows Completed rather than Running while
the success dialog is displayed.

The preferred Windows user-facing package is now a single portable EXE.
It unpacks a verified temporary copy of the runtime (the embedded Python,
native converter, unsigned iOS template and the privately provided language
catalog) on launch, then runs the existing native Builder GUI. Closing the
GUI removes its extracted runtime. This is a portable, self-contained
launcher, not an installer and not a single standalone Windows process.
Windows 11's built-in tar.exe is used locally. The 1.8.1g original game ZIP
must still be supplied by the user. A private hu.xlf included by the
developer appears as a checked language option and becomes compiled iOS
language data only when that option is selected;
the iPhone app does not need any external .xlf.

Two Windows build profiles are maintained. The standard release candidate
does not include UTM/SPICE test transport, while the separately compiled
private UTM test kit retains it for validating shared original ZIPs. Test
kits must not be offered as standard release downloads.

Use the independent [Sideloadly application](https://sideloadly.io/) to sign
and install the generated IPA on an iPhone or iPad. Sideloadly is not bundled
or maintained by this project. A 2026-10-12 Windows 11 ARM user test on
iOS 27 successfully signed and installed an unsigned native IPA using
Sideloadly; the user confirmed gameplay, sound, saves, Hungarian text and
the original icon. A follow-up private Windows 11 ARM test on 2026-10-13 also produced
an unsigned IPA, installed it with Sideloadly v0.70.1, and confirmed that
the game launches and Hungarian text works. On-device switching between
all built-in languages was not exhaustively retested.
This does not guarantee support for every device/version.
The app requires iOS/iPadOS 17 or later. Changing a package's minimum OS does
not make unsupported versions compatible.

A private IPA contains the original game engine and assets. Never upload a
completed IPA to a public GitHub release, issue, or public sharing service.
A distributable Windows template and source-derived metadata require a legal
review before publication. The current Mac installer remains unchanged.

## Set up Windows

1. Download Sideloadly only from [its official website](https://sideloadly.io/).
2. Follow that website's current Windows prerequisites. Its instructions
   require the **web versions of both iTunes and iCloud**, not the Microsoft
   Store versions. Use its Apple download links rather than third-party driver
   packages. If replacing installed Apple software, review its effects on your
   existing setup; this project does not remove or reconfigure that software.
3. Connect the iPhone or iPad by USB, unlock it, and accept **Trust This Computer**.
4. Enable Developer Mode on the device when required and approve the developer
   under Settings > General > VPN & Device Management if prompted.

Do not disable Windows security protections or run a download flagged as
malware. Consult the official Sideloadly support information for installation
or security errors.

## Sign and install

1. Open Sideloadly and select the connected device.
2. Drop the private IPA into its archive area.
3. Use its **Apple ID sideload** mode and enter your own Apple Account. Complete
   authentication or two-factor approval when requested.
4. Start sideloading and keep the device connected and unlocked until it finishes.
5. Open the game and check the intro, portrait orientation, menus, and gameplay.

Apple ID sideloading re-signs the IPA for the selected device using your account.
The device does not need to be included in the Mac export's original profile
when a new valid profile is created by Sideloadly. Its **normal install** mode
instead relies on the existing signature and profile; it cannot fix an expired
profile or an unregistered device.

Enter credentials only into a tool you trust. This project's installer neither
handles that login nor controls Sideloadly's credential storage. Do not enable
tweak injection, minimum-OS overrides, or restriction-removal options for this
runtime. No jailbreak or JIT-enabling step is required.

Use a consistent final bundle identifier and signing account when refreshing
the same app. A different identifier installs a separate app with separate data.
Changing signing teams may prevent an in-place update; back up important data
before changing signing identity. Do not uninstall an existing game to resolve
an error without considering the loss of its local save.

## Expiration and troubleshooting

Free-account sideloads expire after seven days. Re-sign with the same account
and identifier before expiration, or configure Sideloadly's optional automatic
refresh. Automatic refresh depends on its own service, computer availability,
and device connection; this project does not run that service.

Use [Sideloadly's FAQ](https://sideloadly.io/faq) for account limits, pairing,
drivers, signing errors, and refresh configuration. Review its logs before
sharing them: they can contain account, device, and signing information.

The Mac installer tests are separate from the Windows sideload workflow.
A physical-device test using UTM Windows 11 ARM, USB forwarding and Sideloadly
v0.70.1 successfully installed and ran the unsigned IPA on an iPhone with
iOS 27. The user also reported repeated extended gameplay without stability
problems. Signing renewal and installation on additional Windows PCs and
iPads remain untested. The redesigned graphical Windows builder has not yet had
its full interactive end-to-end device test.
