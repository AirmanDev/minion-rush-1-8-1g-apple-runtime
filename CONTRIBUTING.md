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

- The portable core is C11, Apple platform code is Objective-C with ARC, and
  tools use Swift, Python 3, or Bash.
- Use English for source identifiers, comments, user-facing tool output, and
  documentation.
- Comments should explain only non-obvious invariants, formats, or design
  constraints.
- Follow `.clang-format`, `.editorconfig`, and the conventions of the module.
- Do not commit device logs, generated headers, editor metadata, personal
  paths, signing data, or device identifiers.
- Add every maintained text file to the sorted `config/source_manifest.txt`.
- Extend a shared implementation instead of adding a parallel copy.

## Validation

Asset-free checks:

```bash
bash -n ./*.sh tools/*.sh
python3 -m unittest discover -s tests -v
python3 tools/validate_source.py
./tools/format_source.sh --check
```

Complete local validation with your own 1.8.1g assets:

```bash
./build.sh
./test.sh
```

iOS platform changes require `tools/test_ios_simulator.sh` and physical-device
testing before release. State the commands, device class, and result in the
pull request without disclosing device IDs or signing data.

## Contribution license

Unless explicitly stated otherwise, contributions submitted for inclusion are
licensed under the Apache License 2.0, as described by section 5 of `LICENSE`.

## Change structure

ABI and platform boundaries are intentional. Put shared behavior in the
portable core, Apple APIs in the relevant platform file, and game symbols only
in the declarative `game_symbols.def` table. Enforce any new correctness
invariant in a validator or test.
