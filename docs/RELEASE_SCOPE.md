# Portable Builder release scope

## Builder 1.2.6 (native translator diagnostic channel)

The native ARM32-to-ARM64 compiler is a child process of the desktop Python
backend. Its stdout and stderr must not enter the Mac UI's JSON Lines protocol
pipe directly. Unstructured compiler messages, including the normal
`discovery` statistics, previously caused the Swift JSON decoder to report
a false error and abort the build.

Host compiler and translator processes now capture their combined diagnostics
and forward the result through the existing structured log emitter. The
process return code is checked independently; a failing translator still
fails the build and its diagnostics remain visible in the activity log.
The same capture function is used for Mac and Windows native AOT preparation.

Regression tests verify successful diagnostics arrive only as typed log
events and nonzero exits retain their status. The user-supplied original
archive, generic iOS template, optional language packs, ARM64 output format,
and device installation workflow are unchanged.

## Builder 1.2.5 (macOS appearance compatibility)

The native macOS popup menus for selecting a device and Apple signing team
now use the application's established light control appearance in both light
and dark macOS modes. Their SwiftUI labels and fields use the same explicitly
defined foreground and background palette as the rest of the Builder.

The 1.2.4 device discovery behavior is unchanged. A native layout test now
checks a device list updated to two entries after initial presentation, and
verifies selection, popup geometry, and Aqua effective appearance under a
dark parent window. Mac and Windows versions still derive from one Info.plist.
No AOT, game data, additional language, or signed-device deployment changes
are included.

## Builder 1.2.4 (desktop interface and device refresh)

The complete portable Mac and Windows Builders are available as separate
downloads from GitHub Releases. This update keeps the native ARM64 engine,
AOT compiler, source ZIP validation, independent optional-language catalog
and IPA format unchanged.

- The desktop version appears in the Mac header and Windows header. The Mac
  bundle Info.plist is the single source for the generated Windows version.
- The portable Mac app bundles its own NativeKit, Python runtime and
  target-only Hungarian catalog; the source-only Xcode importer does not.
- The Mac device Refresh action runs a direct Xcode devicectl query without
  copying the original game or rebuilding the source runtime. The device
  selector has a visible native border; refreshed device state does not
  discard a previously generated IPA path.
- Physical iPhone and iPad devices reported by Xcode may be selected for the
  optional Xcode signing workflow. Creating an unsigned IPA remains independent
  of Xcode, devices and Apple signing.
- Compiler and language data remain identical to the 1.2.3 release.

## Candidate 1.2.3 (packaging and license attribution)

The next portable Builder candidate supports the same local workflow on both
platforms: a user imports their own unmodified Minion Rush 1.8.1g ZIP and
receives an unsigned IPA containing ahead-of-time translated ARM64 code.

- Mac: ad hoc signed, self-contained arm64 SwiftUI .app.
- Windows: self-contained x64 native Win32 .exe, also on Windows 11 ARM via
  x64 emulation.
- iOS/iPadOS deployment minimum: 17.0. No iOS JIT, ARM32 interpreter or
  on-device code generation.
- The host-only Python runtime, native AOT compiler and generic zero-code
  iOS executable template are included in each desktop Builder.
- The original game ZIP, APK, game assets, game-derived executable blocks,
  address-map data, and private translation XLIFF are NOT included.
- The Builder interface supports English and Hungarian. The original game's
  available languages come from the user-supplied ZIP.

The common toolchain compiles the owner's ARM32 engine into ARM64 instructions
on the desktop. The same host tool creates a v3 binary profile containing
the user's engine bindings, offline hooks, and native block-map offsets.
The iOS runtime links a zeroed 192 MiB code slot without knowing any
game-specific addresses. The unsigned IPA assembler inserts the generated
ARM64 instructions and the profile along with the user's own game assets.
The runtime validates the profile before executing already compiled blocks.

The profile format, bounds and ABI are documented by:
`config/native_profile.json`, `tools/native_profile.py`,
`tools/build_generic_bindings.py`, `src/native/game_profile.c`,
and `src/native/game_profile.h`. Only version 3 is accepted. There is no
fallback to the game-derived v2 template.

### 1.2.3 release packaging changes

This release candidate updates license attributions, desktop title,
unaffiliated compatibility wording and an optional target-only Hungarian
language pack. The original game's language tables remain intact, and the
new table is generated when the user builds the IPA. The iOS engine,
loading overlay and ARM64 translation format are unchanged. The upstream MinGW-w64 runtime licensing text is bundled intact,
with its original copyright signs and a pinned SHA-256. Package validation
rejects a missing or altered notice in both Mac and Windows bundles.
The third-party Python dependency inventory now also carries pinned upstream
notices for OpenSSL, mpdecimal, Expat, liblzma, Zstandard and LibTomMath, plus
Mac-specific libffi and bzip2 notices. These are license-only additions, not
iOS runtime changes. The originals reviewed are ASCII and require no new
source style exemption. Only the existing MinGW-w64 upstream notice is exempt
from the ASCII source rule.

The GCC 16.2.0 toolchain-generated Windows executables import only Windows
system and UCRT DLLs. The compiler's static runtime contribution is governed
by the GCC Runtime Library Exception 3.1 where applicable. Unmodified GCC
GPLv3 and runtime-exception notices are now pinned and included. Original
engine identifiers remain in the compatibility symbol table because the
Builder needs them to resolve symbols within the user-provided ZIP; no
precomputed addresses are distributed. The app's desktop display title is
"Unofficial IPA Builder" and its subtitle identifies Minion Rush 1.8.1g
only as the supported input.

The owner reported that the 1.2.2 Windows-built IPA launched, displayed
its loading overlay and ran on a physical iPhone. No iOS engine or AOT change
was introduced by the notice and independent-branding updates. The actual 1.2.3 packages
were subsequently built and validated on Mac and Windows, including the
entire original-ZIP-to-unsigned-IPA process.

The publisher has selected direct GitHub download with an ad hoc-signed
Mac Builder and an unsigned Windows EXE. The optional Developer ID,
notarization and Authenticode release paths are out of scope. The built
Mac app is rejected by Gatekeeper before manual approval, and the Windows
1.2.3 EXE reports NotSigned in Get-AuthenticodeSignature. Downloaders must
be informed about operating-system trust prompts. User approval is not
available on every restricted or managed device.

The packaged 1.2.3 Mac Builder passed ZIP-to-IPA conversion and generated
1,369 entries and 942,744 native blocks. The 1.2.3 Windows EXE passed
its SHA-256 and self-test directly on the UTM Z: share. Its interactive
Create IPA action then produced an unsigned 367,839,044-byte IPA, independently
verified with the v3 profile: 109 bindings, 942,744 blocks, and
164,353,244 bytes of ARM64 code. The guest test IPA and scheduled task were
removed afterward. The Z: share now contains only the 1.2.3 Builder and
unaltered original ZIP.

### 1.2.2 actual-stage loading view

On iOS, a native UIKit overlay appears immediately after attaching the
scene. It displays an activity spinner and truthful startup milestone text:
source game data, graphics initialization, engine initialization, and the
first drawn frame. Milestones are emitted directly from the game runtime;
no fabricated percentage, timer-based completion or additional network
requests. An actual OpenGL draw call is required to dismiss the overlay
(with a short fade). If engine initialization fails, the overlay presents
a restart instruction instead of silently disappearing. The initial
message also supports Hungarian when the iOS preferred language is hu;
this is Builder-authored text, not a redistributed translation catalog.
The unmodified Mac desktop gameplay implementation has a no-op handler.

An isolated iOS 27 Simulator test of the source-change successfully showed
the loading view during startup and then gameplay frames after it faded.
The corrected responsive width prevents the title being truncated.
The full 1.2.2 Windows GUI-to-IPA regression passed on the UTM Windows 11
ARM guest with x64 emulation: the native Create IPA action generated
367,839,044 bytes from the user's local original ZIP; a separate Windows
ZIP/profile audit confirmed 1,369 entries, v3, 109 bindings, 942,744 AOT
blocks and 164,353,244 ARM64 code bytes. Windows onefile --self-test passed.
The owner subsequently confirmed that the 1.2.2 loading screen and
gameplay work on a physical iPhone. The owner later confirmed sound and
save retention on an iPhone 17 Pro.

### 1.2.1 startup defect and Simulator regression

The initial 1.2.0 IPA starts a UIKit scene but remains black: the portable
iOS AOT runtime is built with `MR_RUNTIME_PROFILE=1` while Xcode compiled
the application delegate without it. Thus the delegate never invoked
`mr_game_profile_load`; engine bindings remained zero.

In 1.2.1 the template Xcode build explicitly enables the profile flag
for the delegate too. Both template creation and release packaging reject
executables without the profile loader markers. Synthetic negative tests
cover this regression.

An iOS 18.6 iPhone 11 Simulator build was made from the same generic v3
runtime, filled on the Mac with a real owner-supplied ZIP and loaded in an
isolated simulator. The profile loaded, the engine ran 650 frames with
942,744 native blocks and zero AOT block faults, and the game started
rendering 118 draw calls at frame 267. A simulator screenshot confirmed
visible game animation. A separate, isolated iOS 27.0 iPhone 17 Pro
Simulator showed the game's intro graphics and text: first draw calls
at frame 229, 360 frames completed and zero ARM64 block faults.
Performance on the simulated GPU is not evidence of hardware frame rate.
The screen turns black again when the bounded
`MR_FRAMES` diagnostic session is intentionally completed; this is not
evidence of a normal indefinite gameplay regression.

The 1.2.1 packaged Mac Builder successfully produced another native IPA
from the user-owned ZIP (1,369 entries, 942,744 AOT blocks, no private
Hungarian XLIFF) and its template contained the working startup profile
loader. The final 1.2.1 Windows EXE was copied to the UTM Z: share and
passed SHA-256 verification plus a zero-exit portable self-test directly
from the guest. The previous 1.2.0 exe was removed from the share.

The owner confirmed that their Windows-built 1.2.1 IPA installed and
ran on a physical iPhone, then independently confirmed that the 1.2.2
loading overlay appeared and handed over to gameplay. These observations
are user-reported evidence, not supervised device-side traces. Existing
user saves must not be overwritten without explicit approval.

## Build the candidate locally

Build the generic template once, on a developer Mac with Xcode 27 or later:

```bash
mkdir -p build/ios-generic
./tools/build_ios_template.sh \
  "$PWD/build/ios-generic/MinionRush-generic-template-unsigned.zip"
```

Then build the portable host executables as described in
[INSTALLER.md](INSTALLER.md) (Mac) and [NATIVE_WINDOWS.md](NATIVE_WINDOWS.md)
(Windows). The portable release packager refuses an existing output folder:

```bash
mkdir -p build/portable-releases
python3 tools/package_portable_builders.py \
  --app "$PWD/build/ios-generic/Minion Rush IPA Builder 1.2.3.app" \
  --windows-exe "$PWD/build/ios-generic/MinionRush-Builder-Windows-1.2.3.exe" \
  --version 1.2.3 \
  --output "$PWD/build/portable-releases/1.2.3"
(cd build/portable-releases/1.2.3 && shasum -a 256 -c SHA256SUMS.txt)
```

The packager verifies the Mac bundle's version, signature (including after
extraction), runtime and kit integrity, the Windows EXE embedded archive,
and the generic template version. It rejects private XLIFF and finished
game IPAs. It does not upload anything.

The open rights, third-party notices and distribution instructions are tracked
in [THIRD_PARTY_AND_RELEASE_GATE.md](THIRD_PARTY_AND_RELEASE_GATE.md).

## Validation status

The generic iOS runtime has built with Xcode 27. The original
user-supplied ZIP was converted to a structurally valid unsigned ARM64 IPA
using the generic template and also with the embedded runtime of the
self-contained Mac Builder, without a bundled Hungarian game catalog.
The profile contains 942,744 generated blocks and 109 engine binding values;
these are generated by the user's own desktop compilation, never shipped in
the generic Builder.

On 2026-10-13 the exact 1.2.0 portable Windows EXE was exercised inside
an interactive Windows 11 ARM UTM guest using x64 emulation. The portable
launcher and internal native GUI both passed `--self-test` after replacing
raw `wWinMain` tail comparison with shared Windows argument parsing.
The EXE opened a responsive visible Builder window, and the owner-supplied
original ZIP was successfully imported through the GUI's Create IPA action.
The resulting unsigned IPA was verified on Windows: 1,369 ZIP entries,
942,744 native AOT blocks, 164,353,244 bytes of precompiled ARM64 code, no
private XLIFF, and no ZIP corruption. The desktop-only conversion took about
41 seconds in the VM. The owner ZIP was copied to the Windows local disk
before the test because its UTM WebDAV drive cannot open archives of that size.

The shared IPA ZIP compression level is now 3. Compared with the old level-1
archives, native IPA size decreased from 376,741,203 to 364,981,565 bytes
on Mac and from 447,936,121 to 367,832,694 bytes on Windows, without
changing the native code, profile or game asset contents. The remaining
platform-dependent PNG icon bytes and zlib implementation explain why these
IPA files are not byte-identical across the platforms.

The owner has confirmed physical iPhone gameplay, sound, save retention
and the 1.2.2 loading screen on an iPhone 17 Pro. Broader iOS and iPad
device coverage remains unverified. A first-launch check on a clean desktop is recommended
to verify accurate trust-prompt instructions, not to demand publisher
signing. The inventoried runtime dependencies have pinned notices;
case-specific rights to transform a user-supplied game remain fact dependent.

## Legal and distribution limits

The generic portable Builder reduces the direct game-material redistribution
risks of v1.1.0. It is not a legal guarantee for the creation or use of a
translated copy of the game. The publisher may hold rights in original
machine code, text, artwork and gameplay assets included in the user's own
generated IPA. The current code also describes engine symbol interfaces and
identifiers. Whether an interoperability or lawful-use exception applies
depends on the actual acts and their necessity; local processing and owning a
copy do not automatically establish permission.

The private Hungarian XLIFF contains 2,483 source/target units and is not
included in the public packages. The candidate includes a source-stripped,
target-only Hungarian JSON pack. It is compiled locally against the user's
original text table as an optional additional language. The translated text
itself can still have derivative-work or other rights restrictions and is
not represented as legally cleared by stripping the English source.

The source code's Apache 2.0 license covers only contributions whose authors
can license them. It does not license the game, its source assets, or the
mark, and signing with Sideloadly/Xcode does not grant those rights. The identified runtime dependency licenses have been inventoried and
packaged. The game's historical EULA and Hungarian/EU interoperability
exceptions remain context dependent. The release must describe this limitation
without claiming Gameloft endorsement or blanket legal authorization.

Primary references:

- https://eur-lex.europa.eu/legal-content/EN/TXT/?uri=CELEX:32009L0024
- https://eur-lex.europa.eu/legal-content/EN/TXT/?uri=CELEX:62010CJ0406
- https://njt.hu/jogszabaly/1999-76-00-00

## Existing public Release

The already published GitHub 1.1.0 RC1 is a separate, Xcode-based Mac
import/sign/install application. It does not contain the v3 generic portable
runtime or Windows EXE. Do not replace or mislabel it before the 1.2.3
candidate's remaining gates are passed. Do not publish any user ZIP,
generated IPA, private translation, signing credentials or device identifiers.
