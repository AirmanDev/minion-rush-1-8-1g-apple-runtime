## Purpose

Describe the problem and the chosen module boundary.

## Validation

List the commands and device classes used for validation.

## Checklist

- [ ] The change targets the historical 1.8.1g runtime.
- [ ] No game binary, DLC, save, signing data, device ID, or personal log was added.
- [ ] `python3 -m unittest discover -s tests -v` passes.
- [ ] `python3 tools/validate_source.py` passes without copyrighted assets.
- [ ] `./test.sh --static` and `./test.sh` pass with local 1.8.1g assets.
- [ ] New behavior and public interfaces are documented.
- [ ] No parallel obsolete path or duplicate solution was added.
- [ ] I have the right to submit this contribution under Apache-2.0.
