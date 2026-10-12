# Legal and distribution review

This is an independent preservation and interoperability project for the
historical Minion Rush 1.8.1g Android release, not the current Unity game.
It is not affiliated with or endorsed by Gameloft, Illumination or Universal
Products & Experiences. "Minion Rush" identifies the game being studied; it
is not a claim of ownership or official status.

This review records issues and release decisions, not a legal permission or
legal advice. Copyright exceptions depend on the facts, jurisdiction, and
the specific acts performed. The technical tests cannot resolve legal rights.

## What the public Apache License 2.0 covers

The public source is offered under the Apache
License 2.0. It covers original project contributions only: independent runtime,
translator, platform adapters, tools, tests, and documentation. Contributors
cannot license third-party game code, characters, names, designs, artwork,
audio, scripts or translations that they do not own. Apache License 2.0
section 6 does not grant trademark rights.

The public source tree may contain original tool code, hashes and format
descriptions. A hash is a technical identifier, not a license to extract,
modify, reproduce or redistribute the underlying work.

## What remains private

Do not commit or distribute any original APK, ARM32 engine, JPK archive,
DLC, graphics, sounds, videos, fonts, save, complete IPA, private XLIFF
catalog, game-derived icon or signing material. The GUI only processes a
locally supplied original ZIP and emits a private unsigned IPA. It does not
download the game or handle Apple credentials.

The Windows iOS template contains no original game binaries or assets and
reserves a zero-filled executable code section. However, the embedded
engine-derived block map, addresses, bindings and generated layout may
still raise copyright or database-related questions. "Asset-free" and
"zero-filled" are technical properties, not a legal clearance. Do not
publish that template, a full portable kit containing it, or a prebuilt
per-user IPA while the review is open.

## Contractual restrictions

Gameloft's published End-User License Agreement restricts reverse
engineering, decompilation, modification, copying and distribution.
Its published Terms and Conditions also prohibit unauthorized mods and
circumvention of service protections. Check which terms applied to the
specific historical 1.8.1g release and acquisition channel; modern
website terms alone do not establish that historical contract.

Neither personal ownership of a copy nor non-commercial preservation
automatically authorizes adaptation or redistribution. No claim is made
that the compatibility build is approved by a rightsholder.

Sources:
- https://www.gameloft.com/en/eula
- https://www.gameloft.com/en/conditions-of-use

## EU and Hungarian software rules

Directive 2009/24/EC Articles 5 and 6 and Hungary's Copyright Act
(1999. evi LXXVI. torveny) sections 59 and 60 provide limited rules for
lawful software use, observation, backup and interoperability. Section 60
addresses indispensable code reproduction or translation needed to obtain
information for an independently created program to interoperate, subject
to conditions on lawful use, information availability, the scope of
decompilation, purpose, sharing and creation of substantially similar
programs.

These provisions are not blanket permission to convert an entire game,
circulate engine-derived maps, publish reconstructed executables or share
original assets. Whether a specific native translation or template fits
an exception needs specialist, fact-specific analysis. Technical measures,
contract terms, and other protected game content require separate review.

Primary sources:
- https://eur-lex.europa.eu/legal-content/EN/TXT/?uri=CELEX:32009L0024
- https://njt.jog.gov.hu/jogszabaly/1999-76-00-00

## Other release requirements

The portable Windows kit includes the official Python embeddable runtime.
Retain its license file and applicable upstream notices when distributing
it. The source compiler and host tools remain subject to their own license;
review all bundled binary dependencies before a release.

A public Windows executable or kit also needs provenance, SHA-256 release
checksums, malware screening, and an appropriate publisher-signing plan.
An unsigned executable may trigger SmartScreen or Smart App Control;
users must not be instructed to disable system protections. Sideloadly is
independent third-party software, not bundled or audited by this project.

Sources:
- https://wiki.python.org/moin/PythonSoftwareFoundationLicenseFaq
- https://learn.microsoft.com/windows/apps/package-and-deploy/smartscreen-reputation
- https://sideloadly.io/

## Release gate

Before offering a downloadable Windows kit or iOS template publicly:

1. Obtain qualified review of the historical license, legitimate source
   acquisition, interoperability analysis, and derived map/binding metadata.
2. Confirm permission or a documented legal basis for every redistributed
   component and any identifying marks used in distribution.
3. Inspect the complete binary inventory, licenses, code-signing policy,
   build provenance and source-to-binary correspondence.
4. Test the resulting kit on supported Windows machines and iOS devices,
   including signing renewal, data retention and long-duration operation.
5. Publish only a reviewed, asset-free package. Never publish completed IPAs,
   private game data or private logs.

Until these checks pass, the new Windows workflow is a local, experimental
development feature, not a cleared public software release.
