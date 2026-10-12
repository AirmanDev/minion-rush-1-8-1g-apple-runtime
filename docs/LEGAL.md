# Legal and distribution boundary

This is an independent preservation and compatibility project for the
historical Minion Rush 1.8.1g Android release. It is not source code for, or a
modification of, the current Unity-based Minion Rush client. It is not
affiliated with Gameloft, Illumination, or Universal Products & Experiences.

The Minion Rush name, Despicable Me setting, Minion characters, original
engine, graphics, audio, levels, DLC, and other game data belong to their
respective rights holders. The repository's source license does not grant
permission to use or distribute them.

## Allowed in the public repository

- Original runtime, translator, platform, and tooling code
- Original documentation and tests
- Checksums and format descriptions needed to identify original files

## Excluded from the public repository

- APK files, ARM32 engine binaries, and modified copies of those binaries
- JPK archives, DLC, graphics, audio, video, fonts, and saved games
- XLIFF files containing original or translated game text
- Original or modified Minion depictions and icons derived from the game
- Certificates, provisioning profiles, device identifiers, and personal logs

The original repository material listed above is available under the Apache
License 2.0. That license applies only to material for which the repository's
contributors can grant rights. It does not license or alter the ownership of
the original game, engine, assets, characters, names, or trademarks.

Users construct the ignored `assets` directory locally from their own lawfully
obtained 1.8.1g release. `.gitignore` excludes this directory and all build
products. Repository owners must review their own legal position before
publication. This document is not legal advice.

## Windows GUI and release rights

The Windows application package contains an unsigned, asset-free iOS template,
not a playable IPA. The user privately imports the original game ZIP. Private
translations are optional and are not distributed with the public kit.

The asset-free template nevertheless contains source-derived block maps
and binding addresses. These may still raise copyright or contractual
questions. Permission or an independent legal assessment is required
before public redistribution. Gameloft's terms restrict copying,
modification, derivative works and distribution of protected materials.
The project's Apache-2.0 license does not override those restrictions.

A public Windows Release additionally needs Windows GUI testing, a release
integrity review and an appropriate Windows executable signing approach.
Windows SmartScreen may warn about an unsigned application. The private
ZIP, generated IPA, community translation catalogs and Apple credentials
must not be uploaded to the release.

Gameloft: https://www.gameloft.com/en/conditions-of-use
Microsoft SmartScreen: https://learn.microsoft.com/en-us/windows/apps/package-and-deploy/smartscreen-reputation
