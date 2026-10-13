# Public Builder distribution scope

## Official download

The initial public release is **Unofficial IPA Builder v1.2.8**:

- [GitHub Release](https://github.com/AirmanDev/minion-rush-1-8-1g-apple-runtime/releases/tag/builder-v1.2.8)
- `MinionRush-Builder-macOS-arm64.zip`: portable Apple Silicon Mac app, ad hoc signed
- `MinionRush-Builder-Windows-x64.exe`: standalone unsigned Windows x64 Builder
- `SHA256SUMS.txt`: verified SHA-256 hashes for both packages

The GitHub Releases page is intended to contain only this Builder version.
The repository's source-control history is retained, but earlier internal
desktop package builds and the source-only Xcode importer are not distributed
as separate public downloads.

## What the Builders do

Both Builders take an original Minion Rush 1.8.1g ZIP supplied by the user
and perform local ARM32/Thumb to ARM64 ahead-of-time compilation.
They assemble a native, unsigned iOS IPA with a validated v3 profile.
No Android emulator, on-device JIT or online game service is required.

The Mac Builder offers **Install Game**, which builds the IPA, checks
the user's existing local development certificate and provisioning profile,
signs the app and installs it to the selected Xcode-paired device.
**Save IPA** is an optional unsigned export; selecting an export destination
is not necessary for Install Game. The Mac signing Team and bundle ID are
advanced settings. Use the same bundle ID on later installations to preserve
the app's save data.

Windows offers **Create IPA**. Windows users must sign and install the
resulting unsigned IPA separately. The two desktop editions share the
native compiler, generic iOS template, English/Magyar desktop labels and
additional-language packaging where applicable.

The Hungarian game-language pack has 2,483 translated targets. It contains
no original English source strings and is compiled locally as a separate
language table, leaving the original languages available. The original
game ZIP and generated IPA do not leave the user's machine as part of the
Builder workflow.

## What is not distributed

Neither the repository nor the public Builder assets include an original
game ZIP or APK, game engine binaries or assets, a finished IPA, a user's
save files, Apple account credentials, private signing material,
a private editable XLIFF, game-derived ARM64 executable blocks or
precomputed binding profiles. JNI and engine lookup identifiers required
for compatibility exist in the source code.

The source-only Xcode importer can be built for development but is not a
third public download.

## Validation record

- The Python suite passed 183 of 184 tests; one test was skipped.
- Native Mac process and layout checks passed, including UI appearance
  and activity-log layout while compilation messages are displayed.
- All four required GitHub checks passed for the release source.
- The portable Mac Builder completed local game translation and unsigned
  IPA assembly with the user-supplied original ZIP and validated the
  additional Hungarian table.
- A complete native Mac install flow was confirmed by the owner on a
  physical device, with the game running and saves retained. Earlier direct
  Mac signing checks also verified the local code signature before
  device installation.
- The Windows 11 UTM virtual-machine self-test passed. Both downloadable
  packages were retrieved again from GitHub and matched their local
  SHA-256 values.
- Broader device/OS coverage, clean-machine first launch and uncommon
  signing, cancellation and recovery scenarios remain potential tests.

## Signing and trust

The Mac Builder is ad hoc signed, not Developer ID signed or notarized.
The Windows executable is not Authenticode signed. Downloads can trigger
normal operating-system warnings. Confirm the release source and checksum,
then follow the operating system's per-app trust controls if appropriate.
Do not disable security protections globally.

The resulting iOS app needs separate signing. The Mac's Install Game
action uses an already configured local development identity and an
iOS provisioning profile covering the selected device and bundle ID.
When no matching profile exists, Xcode automatic signing may be required
once before installation.

## Rights and limitations

This is an independent, unofficial compatibility tool. It is not
endorsed by Gameloft or the game's other rightsholders. The user's
local acquisition and transformation of the original game must be
assessed under applicable laws and contracts. The public Hungarian
target-only translations may also require independent redistribution
rights. Excluding original English text does not establish ownership
of a translation.

The distribution is a tool, not the game. Do not publish a user-supplied
original ZIP, completed IPA, game assets, private translations, Apple
credentials or save files. See [LEGAL.md](LEGAL.md) and
[THIRD_PARTY_AND_RELEASE_GATE.md](THIRD_PARTY_AND_RELEASE_GATE.md).
