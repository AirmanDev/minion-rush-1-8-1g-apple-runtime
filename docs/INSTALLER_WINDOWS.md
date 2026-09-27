# Windows IPA installation with Sideloadly

This project does not provide a Windows installer. Use the independent
[Sideloadly application](https://sideloadly.io/) to sign and install a privately
exported IPA on an iPhone or iPad. Sideloadly is not bundled with this project
and is not maintained by its contributors.

Windows cannot build this runtime from the original release ZIP. The Mac
installer's **Export IPA** action still requires Apple Silicon, macOS 26.6+,
Xcode 27, an Apple Account configured in Xcode, and a connected target device.
The game requires iOS or iPadOS 17 or later; changing an IPA's minimum OS does
not make unsupported versions compatible.

## Prepare a private IPA

1. On the Mac, import your lawfully obtained, unmodified Minion Rush 1.8.1g ZIP.
2. Connect and select an iPhone or iPad. Configure your Xcode signing team and
   bundle identifier as described in [INSTALLER.md](INSTALLER.md).
3. Choose **Export IPA** and save the archive.
4. Transfer it privately to the Windows computer.

The exported IPA contains proprietary game assets and a development profile
with signing and device metadata. Never upload it to a public release, Git,
an issue, or a public file-sharing service. This repository distributes only
the public runtime source and the asset-free Mac installer.

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

Mac installer tests do not validate Sideloadly or Windows USB drivers. A complete
Windows signing, installation, and refresh cycle has not been tested for this
project. The official site advertises iOS 26+ support, but this project has not
verified Sideloadly installation on iOS or iPadOS 27.
