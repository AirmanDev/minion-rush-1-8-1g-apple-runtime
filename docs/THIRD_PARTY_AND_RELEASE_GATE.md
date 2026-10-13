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
| Apple UIKit, OpenGL ES, CoreMotion, AVFoundation, other frameworks | SDK-linked Apple platform libraries; **not bundled as copied SDK libraries** | Apple SDK and distribution terms govern the developer's build/signing process and redistribution. Check the current Apple Developer agreement for the intended distribution channel. |
| Windows operating-system libraries / MinGW-w64 toolchain | Windows system DLL imports; compiler-built PE launchers | The PE launcher dynamically references Windows DLLs. The installed MinGW-w64 14.0.0 toolchain COPYING notice is retained as `third_party_licenses/mingw-w64-COPYING.txt` and carried in the generic kit. Confirm the complete applicable runtime/startup-object and GCC notices before signing/distributing the final EXE. |
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
- [x] Windows 11 ARM/x64-emulation EXE self-test and native GUI Create IPA on the 1.2.2 candidate; 1,369 entries and validated native v3 profile.
- [ ] Collect evidence of the historical 1.8.1g original ZIP's lawful acquisition and its applicable license terms; evaluate user-side copying/adaptation, including relevant local exceptions.
- [ ] Have qualified legal counsel assess whether the particular AOT transformation, public symbol/interface use and individually generated IPAs are permitted under Hungarian/EU law and any controlling contract.
- [ ] Verify trademark/product naming and rights if the public Builder is advertised under a third-party game name or character artwork.
- [ ] Complete component-by-component attribution and MinGW-w64/embedded Python dependency license review and provide required notices.
- [ ] Determine intended distribution channel and its conditions (GitHub direct download vs App Store); Apple 5.2 rights evidence is an additional gate for App Store submissions.
- [ ] Sign/notarize Mac app with Developer ID if desired; Windows Authenticode signing and clean-machine SmartScreen/Gatekeeper testing, and retain verifiable release hashes.
- [ ] Test the **1.2.2 loading-overlay candidate** on physical iOS; preserve existing game saves and signing identity. Confirm accessibility, restart, orientation, failure handling, and that overlay is removed at the first real draw.
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
- Python licensing: https://docs.python.org/3/license.html
- Mac standalone Python build and licensing: https://github.com/astral-sh/python-build-standalone
- GitHub public-repository licensing: https://docs.github.com/en/repositories/managing-your-repositorys-settings-and-features/customizing-your-repository/licensing-a-repository

These statutory exemptions are fact-dependent; the presence of source code on
GitHub, a completed native build, or a lawful user-owned original by itself
cannot grant third-party rights.
