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

To use the Mac path, export the IPA locally and transfer it privately. To use
the Windows experimental kit, launch the included run-native-ipa.cmd. Drop the original ZIP into the
Windows GUI or select Browse, choose where to save the unsigned IPA, and
select Create IPA. The output goes to that chosen location. The CLI runner
run-native-ipa.ps1 remains available for diagnostic command-line use.

Use the independent [Sideloadly application](https://sideloadly.io/) to sign
and install the generated IPA on an iPhone or iPad. Sideloadly is not bundled
or maintained by this project. A 2026-10-12 Windows 11 ARM user test on
iOS 27 successfully signed and installed an unsigned native IPA using
Sideloadly; the user confirmed gameplay, sound, saves, Hungarian text and
the original icon. This does not guarantee support for every device/version.
The app requires iOS/iPadOS 17 or later. Changing a package's minimum OS does
not make unsupported versions compatible.

The GUI does not sign the IPA, require an Apple ID, or upload the ZIP.
It displays the build status and can cancel the local build. The original
translator and asset validator remain the only build implementation.

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
iOS 27. Renewal of the signature, extended gameplay, and installation on
other Windows PCs and iPads remain untested.
