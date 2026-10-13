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

## Private Windows build

The native Win32 GUI accepts a user's separately obtained original ZIP.
It delegates to the existing local Python validator, AOT translator and
unsigned IPA assembler. The user selects the output location. The optional
hu.xlf catalog stays local. No game content or Apple Account credentials
are bundled or sent to a remote service.

The prebuilt iOS template is described as asset-free and uses a zeroed
code section. It still contains a static game-derived block map and
binding addresses. Such derived information may itself raise copyright
and contractual questions. Technical removal of assets and original
instructions does not establish that the template is cleared for public
distribution. The complete Windows kit includes the template and is
therefore held back on the same basis.

The generated IPA contains the user's private game assets and translated
machine code and cannot be treated as a publicly distributable output.
External sideload signing does not grant redistribution rights.

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
on later use or communication of the information.

Hungary's Copyright Act, Act LXXVI of 1999, sections 59 and 60,
implements specific software exceptions and constraints. In particular,
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
- https://njt.jog.gov.hu/jogszabaly/1999-76-00-00

## Private Mac Builder

The private macOS Builder adds a prebuilt asset-free iOS template and the
locally supplied Hungarian XLIFF file to the public macOS application.
Although it does not embed the original game APK, the template contains
engine-derived metadata. The translated XLIFF can also contain protected
game text. Neither should be treated as freely redistributable.

The private Mac .app includes arm64 CPython 3.14.8 from
python-build-standalone. Retain its CPython license, bundled dependency
licenses and upstream provenance. The pinned archive SHA-256 is verified before
packaging; the embedded Mach-O libraries and outer .app are ad-hoc signed.
This is not notarization or an App Store distribution.

The private .app is ad-hoc signed and is not Apple-notarized. It must not
be placed in a public download or a GitHub release without independent
rights clearance, binary license review and applicable macOS signing and
notarization work. The original ZIP remains separately supplied by the
user. A successful on-device or private build test does not authorize
public distribution.

## Windows binary licensing and distribution

The kit bundles an official Python embeddable runtime. Retain its
license text and applicable copyright/attribution notices and check
every separately bundled dependency before public distribution.
Review the compiler, native GUI, template and archive contents against
a complete file inventory. Do not assume that an Apache license on
this repository licenses third-party runtime binaries.

Windows SmartScreen may warn about a new executable even after valid
publisher signing. Unsigned executables may additionally be blocked
by Smart App Control on some systems. Provide checksums and a verified
publisher identity where appropriate; never instruct users to disable
Windows security protections to run the tool.

Sideloadly is an independent signing and installation utility. Do not
bundle it, claim an association, or collect Apple Account credentials
through the project's GUI.

References:
- https://docs.python.org/3/license.html
- https://learn.microsoft.com/en-us/windows/apps/package-and-deploy/smartscreen-reputation
- https://sideloadly.io/

## Release decision

The public GitHub release may include the project source and the asset-free,
ad hoc signed Mac installer already supported by the public source manifest.
This public installer uses the separate Xcode signing/import workflow and
does not contain the private unsigned iOS template, compiler, Hungarian
catalog or original game data. Its unsigned IPA action is unavailable.
The private portable Mac and Windows Builder binaries must stay off the public
release until the template-derived metadata, language translation and all
bundled third-party component licenses receive appropriate review.

Public Windows GUI and complete private portable-kit publication is on hold
until these issues are resolved:

1. Confirm lawful acquisition, the historical license and the exact
   uses for which copyright exceptions or authorization are claimed.
2. Obtain qualified review of the source-derived block map, binding
   metadata and complete asset-free template before redistributing it.
3. Check licenses and attribution obligations for every shipped binary.
4. Verify build provenance, dependency integrity, checksums, signing
   and absence of private materials in the release archive.
5. Run interactive native Windows GUI tests, Windows x64 and ARM64
   compatibility checks, failure/cancellation/cleanup scenarios, and
   physical-device sideload and signing-renewal tests.

Windows 11 ARM private tests on 2026-10-12 and 2026-10-13 confirmed
unsigned IPA preparation and Sideloadly installation on one device.
The newer GUI also passed local ZIP-to-IPA and overwrite testing.
This does not establish public redistribution rights or compatibility
across other machines and devices.

This is a documented release assessment, not legal advice. Public
distribution needs appropriate legal review or rightsholder permission.
