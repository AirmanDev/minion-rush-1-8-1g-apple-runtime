# Release scope

## What is ready

The current full private Builders are version 1.1.0.

- Mac: `build/private-releases/1.1.0/MinionRush-Builder-macOS-arm64.zip`
- Windows: `build/private-releases/1.1.0/MinionRush-Builder-Windows-x64.exe`
- Checksums: `build/private-releases/1.1.0/SHA256SUMS.txt`

Both bundle their Python runtime, native AOT translator, unsigned iOS template,
and Hungarian catalog. Neither bundles the original game ZIP or a finished
IPA. The user supplies their own original 1.8.1g ZIP and generates an unsigned
IPA locally. Mac builds are ad hoc signed. Windows output is signed and
installed separately using Sideloadly; Xcode signing remains an optional
Mac-specific feature.

The private Mac 1.1.0 app passed a complete original-ZIP-to-IPA test with an
isolated bundled Python interpreter: 1,371 ZIP entries, including compiled
Hungarian text. The produced IPA was deleted after verification. The Windows portable EXE and its embedded kit passed executable format,
checksum, translation, and template presence tests. The Windows EXE is not
Authenticode signed. Previous private Windows 11 ARM builds passed a
user-reported Sideloadly/iPhone test. A fresh full device test for the new
1.1.0 packaging is still recommended.

To rebuild the two private packages after compiling both Builders:

```bash
python3 tools/package_private_builders.py \
  --app "build/private/Minion Rush IPA Builder.app" \
  --windows-exe "build/windows-release/MinionRush-Builder.exe" \
  --version 1.1.0 \
  --output "$PWD/build/private-releases/1.1.0"
```

The output directory must not already exist. The packager checks the Mac
version, embedded runtime, template and catalog, Mac ad hoc signature after
ZIP extraction, Windows embedded kit and SHA-256 checksums. It does not upload
any files to GitHub.

## Why these packages are not in the public Release

The prebuilt unsigned iOS template is not the finished game and has no
original game assets or translated machine-code instructions. However, the
runtime was linked against approximately 943,000 original-engine-derived
block-map entries and 115 generated game binding symbols. The generated
addresses are technical data, but their distribution as part of a modified
game runtime requires a case-specific copyright and license assessment.

The bundled private Hungarian XLIFF contains 2,483 source text entries and
2,483 translations from the original game's text. Ownership of the translated
wording alone does not establish rights to publish an adaptation or copy
the underlying original text.

EU case C-406/10 distinguishes unprotected program functionality and
programming interfaces from copyright-protected source/object expression.
Directive 2009/24/EC and sections 58-60 of Hungary's Copyright Act address
software interoperability and its conditions. These provisions are not a
blanket license to publish a translated game or redistribute game-derived
binary material.

Authoritative sources:

- https://eur-lex.europa.eu/legal-content/EN/TXT/?uri=CELEX:62010CJ0406
- https://eur-lex.europa.eu/legal-content/EN/TXT/?uri=CELEX:32009L0024
- https://njt.jog.gov.hu/jogszabaly/1999-76-00-00

## Paths to a public full Builder release

1. Obtain appropriate distribution authorization for the prebuilt
   template-derived content and the language catalog, together with a
   third-party license review. Rights to an original game copy do not by
   themselves provide redistribution permission.
2. Alternatively, develop and test an independent generic iOS template that
   receives all game-specific binding and block-map data only after the
   user's ZIP is imported. The current native runtime compiles this metadata
   into the template; removing it requires a runtime and IPA format change,
   not just deleting manifest entries. The Hungarian translation would still
   need authorization, or must be supplied separately by an authorized source.

Until one of these paths has been reviewed, the full Mac and Windows Builder
binaries remain local private packages. The public GitHub Release contains
only the separate asset-free Mac Xcode installer, named accordingly. Its
unsigned Create IPA action is unavailable without the private template.

Do not publish the local full Builder packages, private language catalog,
generated game IPA, or original game ZIP merely by changing GitHub
visibility or attaching them to a draft Release.
