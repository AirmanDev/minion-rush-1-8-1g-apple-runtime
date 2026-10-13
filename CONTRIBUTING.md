# Contributing

This project maintains an Apple Silicon, iOS, and iPadOS runtime for the
historical Minion Rush 1.8.1g Android release. Compatibility with the current
Unity-based game is out of scope.

## Before making a change

1. Read the module boundaries in `docs/ARCHITECTURE.md`.
2. Look for an existing issue or open a short design proposal.
3. Keep each change focused on one problem and integrate it with the existing
   solution.
4. Do not add a second compatibility path, migration, or device-specific
   quality profile.

## Game files

The original engine, DLC, graphics, audio, saves, and APK are not part of this
repository. Do not publish them in commits, issues, releases, or test artifacts.
For local tests, install your own lawfully obtained 1.8.1g copy with
`tools/install_assets.py`.

## Coding rules

- The portable core and native Win32 GUI are C11. Apple platform code is
  Objective-C with ARC. Other tools use Swift, Python 3 or Bash.
- Use English for source identifiers, comments, user-facing tool output, and
  documentation.
- Keep maintained text files ASCII. Use Unicode escapes in tests and code when
  validating Unicode behavior; game translations remain separate private data.
- Comments should explain only non-obvious invariants, formats, or design
  constraints.
- Follow `.clang-format`, `.editorconfig`, and the conventions of the module.
- Do not commit device logs, arbitrary generated build outputs, editor
  metadata, personal paths, signing data, or device identifiers. The generated
  native Builder palette files are the intentional source-controlled exception.
- Add every maintained text file to the sorted `config/source_manifest.txt`.
- Extend a shared implementation instead of adding a parallel copy.
- Keep IPA output checks and atomic replacement in `native_ipa_output.py`
  on both platforms, not in separate Mac/Windows branches.
- Edit Builder colors only in `config/builder_theme.json`, run
  `python3 tools/build_builder_palette.py`, and commit both generated
  native color files. CI checks deterministic equality.
- When changing the Mac private app, preserve the pinned Python runtime
  source archive, ad hoc signature, original-ZIP separation and optional
  Xcode signing path. Do not treat passing builds as public-redistribution
  rights clearance.

Windows changes must keep archive import, AOT translation and IPA assembly
in their existing Python modules. `src/windows/ipa_builder.c` owns only native
file dialogs, worker-process control, progress, cancellation and status UI.
Do not restore an alternative PowerShell or batch GUI, copy proprietary game
data into tests or distribute the unsigned iOS template before review.
Run the Windows workflow, which builds the EXE, checks the portable Python
kit and executes the GUI self-test. Also test drag-and-drop, file selection,
spaces and non-ASCII paths, cancellation and failed builds interactively on
Windows before a public release. Follow the checklist in `docs/LEGAL.md`.

## Validation

Asset-free checks:

```bash
bash -n ./*.sh tools/*.sh
python3 -m unittest discover -s tests -v
python3 tools/build_builder_palette.py --check
python3 tools/validate_source.py
./tools/format_source.sh --check
```

Complete local validation with your own 1.8.1g assets:

```bash
./build.sh
./test.sh
```

Native installer changes also require an asset-free macOS build and process/
model tests:

```bash
./tools/build_installer.sh
./tools/test_installer.sh
```

Download-package changes also require `./tools/package_installer.sh`. It checks
the exact public-file allowlist and verifies the signed app after ZIP extraction.
Do not describe local packaging checks as a clean-Mac Gatekeeper test; record
that separate first-launch check before publishing a downloadable binary.

Keep installer labels, ordering, and signing-input rules in
`config/installer_ui.json`. Do not duplicate import or device-installation logic
in the native client. See `docs/INSTALLER.md` for the UI contract and
`docs/INSTALLER_WINDOWS.md` for external Sideloadly installation. IPA export
changes require the archive tests and Mac packaging checks. Updates to third-party
instructions must cite current official sources and distinguish documented
capabilities from physically tested signing, installation, and refresh results.

iOS platform changes require `tools/test_ios_simulator.sh` and physical-device
testing before release. Keep the deployment minimum in `ios/Deployment.xcconfig`
and guard newer Apple APIs by availability. Use explicit Simulator family
arguments for required coverage; report missing runtimes as untested, not passed.
State the commands, OS versions, device class, and result in the pull request
without disclosing device IDs or signing data.

## Contribution license

Unless explicitly stated otherwise, contributions submitted for inclusion are
licensed under the Apache License 2.0, as described by section 5 of `LICENSE`.

## Change structure

ABI and platform boundaries are intentional. Put shared behavior in the
portable core, Apple APIs in the relevant platform file, and game symbols only
in the declarative `game_symbols.def` table. Enforce any new correctness
invariant in a validator or test.
