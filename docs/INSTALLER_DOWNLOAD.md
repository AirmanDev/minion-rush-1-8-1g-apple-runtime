# Mac Builder download

The public Mac download is **Unofficial IPA Builder v1.2.8**.
The full application is available from the
[Builder release](https://github.com/AirmanDev/minion-rush-1-8-1g-apple-runtime/releases/tag/builder-v1.2.8).

Download `MinionRush-Builder-macOS-arm64.zip` for Apple Silicon,
verify its SHA-256 digest, extract the ZIP, and open the included app.
The Builder contains the native compiler, generic iOS template, embedded
Python runtime and optional additional-language pack. It does not contain
the original game ZIP or a finished IPA.

The application is ad hoc signed. macOS may request an explicit approval
for a downloaded app in System Settings > Privacy & Security. Verify the
publisher source and checksum first. Do not disable Gatekeeper, strip
quarantine or ignore malware warnings.

Choose an original game ZIP and a paired device to use **Install Game**.
A matching local Xcode development certificate and provisioning profile
are required to sign the game for that device. **Save IPA...** instead
exports an unsigned IPA without device installation.

The old source-only Xcode importer is retained only as development code
within the repository, not as a separate public download. To build that
development utility locally, run `./tools/package_installer.sh`.
It is not a substitute for the complete portable Builder.

See [Mac setup and signing](INSTALLER.md), [release scope](RELEASE_SCOPE.md)
and [legal limitations](LEGAL.md).
