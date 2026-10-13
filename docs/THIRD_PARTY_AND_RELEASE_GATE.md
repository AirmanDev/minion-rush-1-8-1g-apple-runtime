# Third-party inventory and distribution record

Review date: 2026-10-13. Scope: the locally generated **generic** Mac and
Windows portable Builder release, not a finished game IPA or the
owner-supplied original archive.

## What is included, and what it permits

| Component | Where it is bundled | Evidence / action |
| --- | --- | --- |
| Project-authored portable UI, CLI, AOT translator, runtime/shims | Runtime source within each Builder; native binaries | Top-level `LICENSE` (Apache-2.0) is included in both bundles. This license applies only to contributions for which the project author has rights; it cannot relicense the game. |
| CPython Windows 3.14.7 embeddable distribution | `python/` within Windows onefile kit | `python/LICENSE.txt` is bundled and enforced by packaging checks. CPython's bundled components are covered by its license file. Check the specific ZIP's origin, license and dependency notices before redistribution. |
| CPython macOS 3.14.8 standalone runtime | `PythonRuntime/` within Mac app | `lib/python3.14/LICENSE.txt` retained and packaging-validated; source is pinned `astral-sh/python-build-standalone` distribution. `ORIGIN.txt` and the build-script SHA-256 identify it. Audit any additional bundled native third-party components/attributions. |
| Apple UIKit, OpenGL ES, CoreMotion, AVFoundation, other frameworks | SDK-linked Apple platform libraries; **not bundled as copied SDK libraries** | Apple SDK and distribution terms govern the developer's build/signing process and redistribution. The desktop Builder is distributed directly via GitHub, not through the App Store. iOS IPA signing remains separate. |
| Windows operating-system libraries / MinGW-w64 / GCC 16.2.0 | Windows UCRT and system DLL imports; compiled PE launchers and host AOT executable | The PE imports were inspected: the project's three x64 executables reference Windows system/UCRT DLLs, not external GCC or winpthreads DLLs. The MinGW-w64 runtime and GCC 3.1 Runtime Library Exception / GPLv3 notices are present and checksum-validated. GCC's exception covers eligible target-code combinations; this does not make the project GPL-licensed. |
| Generic iOS runtime and empty native code slot | `NativeKit/template.zip` / Windows kit template | Does not contain a game ZIP, original ARM32 engine, game asset, generated ARM64 game code, profile bindings or the private XLIFF. The final *user-built IPA* **does** contain locally translated code and original assets. |
| Community Hungarian text targets | `Runtime/language_packs/hu.json` in both Builders | Contains 2,483 translated targets, language identifier and source-table SHA-256, without original English source fields. Converted to a separate game text table only on the user's machine. This remains potentially subject to translation and derivative-work rights. |
| Game symbol names, interfaces, and technical references | `src/native/game_symbols.def`, shim/compatibility source | Interface names can be functional and may fall outside protection as expression under CJEU C-406/10, but their precise provenance, degree of copying, associated contracts and exceptions have **not** been individually legally cleared. They cannot be marked cleared merely because they are identifiers. |
| UI references to Minion Rush, Gameloft and Minions | Original ZIP selection, game compatibility identifiers and technical documentation | The desktop display title is now "Unofficial IPA Builder"; the supported game name is referential only. The independent icon is created from source shapes, not game art. The marks are not granted under the project's Apache license. |

The 1.2.3 binary inventory was reviewed directly, not inferred solely from
build scripts. The Mac ZIP includes Python reporting OpenSSL 3.5.9,
SQLite 3.53.1 and mpdecimal 4.0.0. The Windows onefile embedded kit includes
libcrypto-3.dll, libssl-3.dll,
libffi-8.dll, libtommath.dll, sqlite3.dll and _zstd.pyd. The Windows CPython
LICENSE.txt already includes libffi and bzip2 notices, which are not copied
again as separate Windows-specific license files. Additional upstream notices
for OpenSSL 3, mpdecimal, Expat, liblzma, Zstandard and LibTomMath are now
included in both generic runtime source bundles, with exact SHA-256 validation;
Mac-specific libffi and bzip2 notices are placed under PythonRuntime only.
The original upstream license text is preserved. This inventory does not
establish every static library contribution or the legal suitability of all
software and trademarks for public release.

The x64 compiler is Homebrew MinGW-w64 14.0.0_3 with GCC 16.2.0.
The produced launcher, native UI, and AOT compiler import Windows and UCRT
system DLLs. No separate libgcc_s, libstdc++ or libwinpthread DLL ships.
GCC's default link libraries may still contribute statically linked runtime
code. The unmodified upstream GCC COPYING3 and COPYING.RUNTIME texts are now
packaged and pinned, alongside MinGW-w64 copyright notices. The Windows Python
3.14.7 embeddable archive also brings its own vcruntime140 DLLs and native
extensions; the Builder does not independently bundle a compiler toolchain or
Microsoft runtime installer. These findings cover the inspected 1.2.3 inputs;
new dependencies require a new inventory.

The game symbol list contains JNI and C++ lookup names but no original binary.
The desktop UI now uses an unofficial product name and refers to Minion Rush
1.8.1g as the supported input only. This improves source attribution and avoids
implying Gameloft endorsement; it does not resolve every use right.

Packaging checks ensure the two bundled Python and project license texts are
present and prevent game/translation artifacts from entering generic bundles.
These checks are **technical**, not a substitute for a complete component
license audit or actual redistribution rights.

## Distribution checks and remaining limitations

- [x] Generic portable bundles contain no original game assets, finished IPA, prebuilt translated machine code or editable private XLIFF. The Hungarian target-only pack is bundled separately and may require translation rights.
- [x] Both Builders generate user-specific native ARM64 code locally, no on-device JIT.
- [x] Mac/Windows bundle-integrity tests and source checks; bundled Python and project license notices present.
- [x] iOS 18.6 and iOS 27 simulator rendering tests for the profile-based runtime.
- [x] Windows 11 ARM/x64-emulation EXE self-test and native GUI Create IPA on both 1.2.2 and 1.2.3; 1,369 entries and validated native v3 profile.
- [x] Require a user-supplied original ZIP and document the user's responsibility for lawful possession; never distribute the original game or a finished IPA.
- [x] Document the narrower tool-only redistribution scope and the limits of Article 5/6 lawful-use and interoperability exceptions; no claim is made that every local AOT adaptation is authorized. The original 1.8.1g acquisition contract is not verified.
- [x] Replace desktop product-facing branding with "Unofficial IPA Builder"; keep the game name only as a compatibility reference. Project-generated icon contains no original character art. No blanket trademark clearance is claimed.
- [x] Bundle and checksum-validate the MinGW-w64, CPython and identified additional Python component notices.
- [x] Inventory the shipped Windows/Mac runtime libraries, review compiler-linked system imports and GCC runtime exception, and bundle the additional pinned notices. Recheck if any dependency changes.
- [x] Distribution is direct GitHub Releases download, not an App Store submission.
- [x] Publisher chose an ad hoc-signed Mac Builder and unsigned Windows EXE, without Developer ID, notarization or Authenticode. No further desktop signing work is planned. The iOS IPA still needs signing for installation.
- [x] Verified delivery limitations: macOS Gatekeeper rejected the ad hoc-signed Builder until the user makes an explicit trust decision; Windows Get-AuthenticodeSignature reported NotSigned for the 1.2.3 EXE.
- [x] Document checksum validation, unaffiliated product branding, and normal first-launch approval. Game-specific symbols are retained as necessary lookup keys; legal use depends on context.
- [x] Owner confirmed physical iPhone installation, gameplay, audio and preservation of saves through the Mac Builder's direct-install workflow.
- [ ] Wider device/OS, accessibility, unusual failure handling and clean-machine trust-prompt checks are desirable. A physical iPhone install, launch, audio and save continuity have been verified; do not reset those saves.
- [x] The publisher selected a tool-only GitHub distribution at version 1.2.8, while documenting unresolved game-copy, adaptation and translation rights. No blanket legal clearance, historical acquisition-contract verification or rightsholder approval is asserted.

## Sources reviewed

Primary and first-party legal materials (read in current version and confirm
historical applicability independently):

- EU Directive 2009/24/EC, Arts. 4-8: https://eur-lex.europa.eu/legal-content/EN/TXT/?uri=CELEX:32009L0024
- Hungary Act LXXVI/1999, sections 58-60: https://njt.hu/jogszabaly/1999-76-00-00.42
- CJEU C-406/10, SAS Institute v. World Programming: https://eur-lex.europa.eu/legal-content/EN/TXT/?uri=CELEX:62010CJ0406
- CJEU C-13/20, Top System v. Belgium (lawful-user decompilation for error repair only to the necessary extent): https://eur-lex.europa.eu/legal-content/EN/TXT/?uri=CELEX:62020CJ0013
- Gameloft EULA: https://www.gameloft.com/en/eula
- Gameloft Conditions of Use: https://www.gameloft.com/en/conditions-of-use
- Apple's App Review Guideline 5.2: https://developer.apple.com/app-store/review/guidelines/
- Apple's instructions for third-party-rights documentation: https://developer.apple.com/app-store/review/
- MinGW-w64 runtime notices: https://github.com/mingw-w64/mingw-w64/blob/master/COPYING.MinGW-w64-runtime/COPYING.MinGW-w64-runtime.txt
- Python licensing: https://docs.python.org/3/license.html
- Mac standalone Python build and licensing: https://github.com/astral-sh/python-build-standalone
- EU trademark referential-use limitation, Article 14: https://eur-lex.europa.eu/eli/reg/2017/1001/oj/eng
- GCC runtime exception: https://www.gnu.org/licenses/gcc-exception-3.1.html
- GCC license sources: https://github.com/gcc-mirror/gcc/blob/master/COPYING.RUNTIME
- GitHub public-repository licensing: https://docs.github.com/en/repositories/managing-your-repositorys-settings-and-features/customizing-your-repository/licensing-a-repository

These statutory exemptions are fact-dependent; the presence of source code on
GitHub, a completed native build, or a lawful user-owned original by itself
cannot grant third-party rights.
