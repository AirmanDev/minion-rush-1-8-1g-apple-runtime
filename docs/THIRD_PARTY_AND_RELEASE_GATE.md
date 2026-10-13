# Third-party inventory and distribution decision gate

Review date: 2026-10-13. Scope: the locally generated **generic** Mac and
Windows portable Builder candidate, not a finished game IPA or the
owner-supplied original archive.

## What is included, and what it permits

| Component | Where it is bundled | Evidence / action |
| --- | --- | --- |
| Project-authored portable UI, CLI, AOT translator, runtime/shims | Runtime source within each Builder; native binaries | Top-level `LICENSE` (Apache-2.0) is included in both bundles. This license applies only to contributions for which the project author has rights; it cannot relicense the game. |
| CPython Windows 3.14.7 embeddable distribution | `python/` within Windows onefile kit | `python/LICENSE.txt` is bundled and enforced by packaging checks. CPython's bundled components are covered by its license file. Check the specific ZIP's origin, license and dependency notices before redistribution. |
| CPython macOS 3.14.8 standalone runtime | `PythonRuntime/` within Mac app | `lib/python3.14/LICENSE.txt` retained and packaging-validated; source is pinned `astral-sh/python-build-standalone` distribution. `ORIGIN.txt` and the build-script SHA-256 identify it. Audit any additional bundled native third-party components/attributions. |
| Apple UIKit, OpenGL ES, CoreMotion, AVFoundation, other frameworks | SDK-linked Apple platform libraries; **not bundled as copied SDK libraries** | Apple SDK and distribution terms govern the developer's build/signing process and redistribution. The desktop Builder is distributed directly via GitHub, not through the App Store. iOS IPA signing remains separate. |
| Windows operating-system libraries / MinGW-w64 toolchain | Windows system DLL imports; compiler-built PE launchers | The PE launcher dynamically references Windows DLLs. The general COPYING notice and the separate official runtime attribution (`third_party_licenses/mingw-w64-runtime-COPYING.txt`) are in the generic kit. Packaging enforces the SHA-256-pinned complete runtime notice. GCC/toolchain-specific and other dependencies must still be reviewed before public distribution. |
| Generic iOS runtime and empty native code slot | `NativeKit/template.zip` / Windows kit template | Does not contain a game ZIP, original ARM32 engine, game asset, generated ARM64 game code, profile bindings or the private XLIFF. The final *user-built IPA* **does** contain translated protected code and assets. |
| Game symbol names, interfaces, and technical references | `src/native/game_symbols.def`, shim/compatibility source | Interface names can be functional and may fall outside protection as expression under CJEU C-406/10, but their precise provenance, degree of copying, associated contracts and exceptions have **not** been individually legally cleared. They cannot be marked cleared merely because they are identifiers. |
| UI name and references to Minion Rush, Gameloft, Minions | UI title, docs, sample metadata | Nominative descriptive use differs from using a protected mark as a product identity; assess risk of misleading affiliation and trademark permission. The game, characters, art and marks are **not** covered by the project Apache license. |

Packaging checks ensure the two bundled Python and project license texts are
present and prevent game/translation artifacts from entering generic bundles.
These checks are **technical**, not a substitute for a complete component
license audit or actual redistribution rights.

## Release gate: evidence still required

- [x] Generic portable bundles contain no protected game assets, finished game IPA, prebuilt translated machine code, or private XLIFF.
- [x] Both Builders generate user-specific native ARM64 code locally, no on-device JIT.
- [x] Mac/Windows bundle-integrity tests and source checks; bundled Python and project license notices present.
- [x] iOS 18.6 and iOS 27 simulator rendering tests for the profile-based runtime.
- [x] Windows 11 ARM/x64-emulation EXE self-test and native GUI Create IPA on both 1.2.2 and 1.2.3; 1,369 entries and validated native v3 profile.
- [ ] Collect evidence of the historical 1.8.1g original ZIP's lawful acquisition and its applicable license terms; evaluate user-side copying/adaptation, including relevant local exceptions.
- [ ] Have qualified legal counsel assess whether the particular AOT transformation, public symbol/interface use and individually generated IPAs are permitted under Hungarian/EU law and any controlling contract.
- [ ] Verify trademark/product naming and rights if the public Builder is advertised under a third-party game name or character artwork.
- [x] Bundle and checksum-validate the official MinGW-w64 runtime copyright notices and both embedded CPython license files.
- [ ] Finish independent review of all bundled Python extensions, GCC/toolchain components and any other required copyright notices.
- [x] Distribution is direct GitHub Releases download, not an App Store submission.
- [x] Publisher chose an ad hoc-signed Mac Builder and unsigned Windows EXE, without Developer ID, notarization or Authenticode. No further desktop signing work is planned. The iOS IPA still needs signing for installation.
- [x] Verified delivery limitations: macOS Gatekeeper rejected the ad hoc-signed Builder until the user makes an explicit trust decision; Windows Get-AuthenticodeSignature reported NotSigned for the 1.2.3 EXE.
- [ ] Assess the conditions for direct downloads, including the product name, game-specific interfaces and accurate user-facing first-launch instructions. Manual approval is not guaranteed on managed or restricted computers.
- [x] Owner reported successful physical iPhone gameplay and loading overlay from the 1.2.2 Windows-built IPA.
- [ ] Finish audio, save continuity, accessibility, orientation and failure-path device tests; do not overwrite existing saves without backup.
- [ ] Get final counsel/rights-owner clearance appropriate to the actual distribution plan; **do not represent a release as fully legally cleared before then**.

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
- GitHub public-repository licensing: https://docs.github.com/en/repositories/managing-your-repositorys-settings-and-features/customizing-your-repository/licensing-a-repository

These statutory exemptions are fact-dependent; the presence of source code on
GitHub, a completed native build, or a lawful user-owned original by itself
cannot grant third-party rights.
