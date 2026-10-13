# Copyright, licensing and release review

Review updated: 2026-10-13. Scope: the historical Minion Rush 1.8.1g
compatibility runtime, native Windows ZIP-to-IPA builder, source-derived
iOS executable template, optional translations and distribution packaging.

This is an independent project, not affiliated with Gameloft, Illumination
or Universal Products & Experiences. The Minion Rush name identifies the
historical title being studied, not an official or endorsed product.

## Project source and third-party rights

Original project contributions are published under the Apache
License 2.0. The license covers only code and other contributions whose
authors can grant those rights. Section 6 does not grant trademark
licenses. It cannot authorize copying or distributing third-party games.

The game name, characters, engine, textures, audio, animations, levels,
DLC and other game content remain the property of their rightsholders.
Do not include any game APK, ARM32 engine binary, JPK or DLC archive,
artwork, original launcher icon, soundtrack, private translation XLIFF,
savegame, converted executable, signed or unsigned finished IPA, Apple
certificate, profile, personal log or device ID in the public repository,
release or automated test artifacts.

A SHA-256 engine fingerprint and original file-format description can
support verification. Neither proves permission to use or redistribute
the corresponding protected content.

## Portable generic Windows build

The portable native Win32 GUI imports a user's separately obtained original
ZIP. It uses the bundled Python interpreter and host AOT compiler to
generate all ARM64 machine code, engine binding values and block-map offsets
only on the user's computer, then assembles a local unsigned IPA. The generic
template is compiled without game-derived addresses, game assets, the original
game code or an XLIFF translation. The public candidate contains no source
APK or Apple Account credentials and does not upload user content.

The generated IPA still contains the user's game assets and translated
machine code, and is a distinct legal issue from the Builder software.
Signing it externally does not grant redistribution rights.

## Gameloft license and acquisition history

Gameloft's published End-User License Agreement restricts decompilation,
modification, reverse engineering, copying, derivative works and
distribution. Its current Terms and Conditions also impose restrictions
on unauthorized modifications and protected content.

The web EULA was last updated on 2021-08-30; it is not evidence of
which exact license governed a 2014-era 1.8.1g acquisition. Determine
the historical release's actual contractual terms, jurisdiction and
source of acquisition before a final conclusion. Ownership of a local
copy, no commercial motive and the age of a release do not alone provide
a right to translate or distribute the software.

Sources:
- https://www.gameloft.com/en/eula
- https://www.gameloft.com/en/conditions-of-use

## EU and Hungarian interoperability rules

Directive 2009/24/EC Article 5 addresses limited acts relating to
lawful software use, backups and observation. Article 6 addresses
decompilation only where indispensable to obtain information necessary
to make an independently created program interoperate with others.
The conditions include lawful access, unavailable interoperability
information, limits to necessary program portions and restrictions
on later use or communication of the information. The CJEU in Top System
(C-13/20) also acknowledged narrowly necessary decompilation for error
repair by a lawful acquirer, but did not authorize distribution of a
translated copy of the whole application to third parties.

Hungary's Copyright Act, Act LXXVI of 1999, section 58(2) expressly
includes software rewriting from one programming language into another
among the acts governed by the adaptation provisions. Sections 59 and 60
implement specific software exceptions and constraints. In particular,
section 60 does not permit using interoperability information for an
unrelated purpose or for making a substantially similar program.

These provisions do not automatically authorize wholesale code
translation, a reconstructed game, publicly shared engine-derived
maps or a playable IPA. Whether any precise step qualifies for a
statutory exception requires a fact-specific legal analysis.

Directive Article 8 states that contractual provisions contrary to
Article 6 and the exceptions in Article 5(2) and (3) are void.
Consequently it is also inaccurate to describe every EULA restriction
as necessarily overriding statutory rights. Neither the EULA nor the
exception alone resolves this particular project.

Primary sources:
- https://eur-lex.europa.eu/legal-content/EN/TXT/?uri=CELEX:32009L0024
- https://njt.hu/jogszabaly/1999-76-00-00.42
- https://eur-lex.europa.eu/legal-content/EN/TXT/?uri=CELEX:62020CJ0013

## Portable generic Mac Builder

The macOS portable Builder uses the same asset-free, address-free
iOS v3 template and host AOT assembly as Windows. The pinned arm64
CPython 3.14.8 runtime, C compiler, SwiftUI application and iOS SDK-built
generic template ship locally. The original game ZIP and the private
Hungarian language catalog do not ship in the portable package.

Retain CPython's license and dependent licenses and verify the upstream
archive fingerprint and code signature. The assembled Mac .app is ad hoc
signed; this is not notarization.

A specific inventory and validation gate is described in
[RELEASE_SCOPE.md](RELEASE_SCOPE.md).

## Reviewed rights sources and packaging evidence

The project-specific rights and license-release checklist is maintained in
[THIRD_PARTY_AND_RELEASE_GATE.md](THIRD_PARTY_AND_RELEASE_GATE.md).
The candidate includes CPython, project and upstream MinGW-w64 runtime
licensing notices on Mac/Windows;
review of remaining bundled third-party dependencies and toolchain notices remains open.
The European Court of Justice's judgment C-406/10 distinguishes program
functionality and data formats from protectable source/object-code
expression, but does not make all game-derived interfaces automatically
free to use. Apple's App Review Guideline 5.2 demands third-party rights
for a third-party game submitted through that channel. A standalone
Builder distributed from GitHub is a different product and distribution
channel, yet remains subject to applicable intellectual-property and
contractual rules. A successful user's private iPhone gameplay test does
not prove redistribution rights.

## Release decision

The already-published 1.1.0 RC1 is a distinct, Xcode-based Mac-only
installer. The portable Builder candidate supplies portable native IPA creation
on both Mac and Windows using a generic runtime template without
precomputed game-derived bindings or a bundled private XLIFF.

Removing those bundled materials materially reduces direct redistribution
risks, but does not determine whether every private ARM32-to-ARM64 game
translation, generated IPA, reverse engineering act or historical
contract is permitted. A case-specific legal review is still necessary
before publicly representing full distribution clearance.

A full release requires verified Windows GUI E2E testing, physical
iPhone/iPad gameplay regression, dependency license audit, trustworthy
installer checksums, developer signing/notarization decisions, and
inspection of the remaining publicly exposed engine symbol names.

Do not publish original game ZIPs, generated user IPAs, saved games,
private XLIFF catalogs, or account credentials.
