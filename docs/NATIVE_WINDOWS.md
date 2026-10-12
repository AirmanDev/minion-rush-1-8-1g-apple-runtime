# Experimental Windows native AOT preparation

This is a development milestone, not a finished Windows-to-iPhone installer.
The original macOS and iOS build workflows remain supported and unchanged.

## Scope

The repository includes a host-native ARM32/Thumb-to-ARM64 translator with separate Windows and POSIX host-memory implementations. The new Python preparer accepts a user-owned original 1.8.1g ZIP. It reuses the existing bounded ZIP extraction, official-release hash and asset validation, then builds and runs the translator locally. It outputs private native ARM64 code, block metadata, and original assets. It does not execute JIT on the iPhone.

This preparer has not been validated end-to-end on Windows or iPhone.

## Experimental requirements and invocation

- Windows x64 or macOS, Python 3.11+ and locally installed Clang C compiler.
- Original, lawfully obtained, unmodified Minion Rush 1.8.1g archive.
- Sufficient disk space and a fresh absolute output path outside the public source tree.

Run from the project root:

    python tools/prepare_native_aot.py C:\Private\original-1.8.1g.zip --output C:\Private\native-output

The output contains the validated original engine and game assets under assets/,
the derived ARM64 code under compiled/minion_code.bin, the block map under
compiled/minion_map.inc, engine metadata in compiled/game_bindings.h,
rejected-candidates.tsv diagnostics, and checksums in aot-manifest.json.

THE ENTIRE OUTPUT IS PRIVATE. Do not publish or commit it, share it as a GitHub
release, include it in public CI artifacts, or upload the original ZIP.
The game-derived ARM64 code is not independently redistributable.

## What is still missing

1. A redistributable, asset-free iOS Mach-O runtime template with a guaranteed
   bounded executable region, consistent code offsets and block-map metadata.
2. A safe, verified Windows assembly step that inserts the private translation
   into the template without corrupting Mach-O sections or entitlements.
3. Code signing/provisioning of the changed executable and packaging an IPA on
   Windows without distributing Apple SDK components.
4. A Windows-to-iPhone physical-device installation and runtime test.

The current app embeds native code statically through src/native/game_code.S
in the __MRCODE Mach-O segment. Modifying any signed iOS executable invalidates
its signature. The current Sideloadly documentation does not prove that a
modified binary can be safely re-signed.

## Compatibility and validation

The existing Apple build scripts (build.sh, tools/build_ios.sh, deploy_ios.sh)
are intentionally unchanged. Mac continues to allocate memory using mmap; the
Windows compiler uses native VirtualAlloc. Native ARM64 gameplay on the Mac
and iPhone has no new interpreter or JIT.

Validate with the existing Python tests, source validator, Mac builds, and a
Windows Clang build of the offline translator. A compiler passing the synthetic
ELF test is NOT an end-to-end Windows IPA test.

Legal boundary: docs/LEGAL.md. Public releases must not contain original or
translated Gameloft code or the game's proprietary assets.

## Verified locally

On 2026-10-12, the Mac compiler produced byte-identical ARM64 code, block map,
rejected-candidate report, and binding header compared with the previous Mac
build. The private ZIP-to-AOT preparation pipeline also passed end-to-end
using the local 1.8.1g archive. A Mac static validation run passed 87 tests
and verified the asset tree (1364 files). No Windows build or iPhone install
from this experimental workflow has been verified.
