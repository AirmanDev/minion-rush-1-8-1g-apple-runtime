# Native Windows unsigned IPA Builder

## User workflow

The Windows 11 x64 portable EXE also works on Windows 11 ARM through x64
emulation. It has a native Win32 interface and needs no separately installed
Python, compiler, Xcode, or macOS machine.

1. Open the downloaded Builder EXE. On first launch it extracts its signed-off
   inventory to a private temporary workspace.
2. Drag a lawfully obtained, unmodified Minion Rush 1.8.1g release ZIP into
   the window, or choose it with Browse.
3. Select a local destination ending in `.ipa`. An existing IPA requires
   explicit replacement confirmation.
4. Click Create IPA. The host translates the original ARM32 engine into
   ARM64 machine-code blocks ahead of time and writes an unsigned IPA.
5. Use a separate signing tool such as Sideloadly with your own Apple account
   before installing to an iPhone or iPad.

The output is native ARM64 and retains the original game's data, audio,
artwork and saved-state format. The destination-side atomic writer protects
any earlier IPA if the new build fails. The standard EXE does not use the
experimental UTM SPICE/WebDAV import transport.

The English / Magyar selector changes the Windows **Builder UI**, not the
game. The portable edition includes an optional target-only Hungarian pack and
the original languages from the user's ZIP. No original English translation
sources or private editable XLIFF are shipped. The checked language option
adds Magyar as another game language during the local IPA build. The EXE has
local error diagnostics and log export; logs may contain private file paths
and must not be uploaded publicly.

## How the generic native runtime works

The distributable template is an unsigned ARM64 Mach-O app produced
once by Xcode on the **developer's** Mac. It has a 192 MiB, zero-filled
`__MRCODE/__text` executable slot, and no imported game, static
game-specific binding addresses, machine-code instructions, or game files.

On each user's desktop, the AOT translator processes the validated original
engine and writes `minion_code.bin`, `minion_map.inc` and
`game_bindings.h`. The shared `native_profile.py` tool then packs
the map and binding values into a strictly checked v3 `game-profile.bin`,
which is placed inside that user's IPA. `native_ipa.py` inserts the
generated ARM64 machine code into the zeroed Mach-O slot, adds private
assets and creates an unsigned IPA. All modifications happen before signing.
The iPhone loads the validated profile before gameplay, then executes native
ARM64 blocks. There is no ARM32 interpreter or iOS JIT.

The same source, ABI limits and conversion stages are used on macOS;
there is no parallel Windows-specific implementation of IPA patching.
No compatibility path remains for the earlier game-derived v2 template.

## Build inputs for maintainers

The developer must have Xcode 27+ on an Apple Silicon Mac to produce the
generic template. Neither the game ZIP nor a generated game map is needed:

```bash
mkdir -p build/ios-generic
./tools/build_ios_template.sh \
  "$PWD/build/ios-generic/MinionRush-generic-template-unsigned.zip"
```

The template output must be a new path and is refused if it exists.
For the Windows native UI and one-file launcher, install a Windows x64
cross-compiler with MinGW-w64 and run:

```bash
./tools/build_windows_gui.sh release
```

Prepare the Windows kit with a verified Python 3.14.7 official embeddable
distribution and host-compatible `a64-compiler.exe`. Both artifacts are
developer-side build inputs; their pinned hashes are validated by the kit.
The generic template is the one created above:

```bash
python3 tools/build_windows_kit.py \
  --python-embed-zip "$PWD/build/private-cache/python-3.14.7-embed-amd64.zip" \
  --compiler "$PWD/build/private-cache/a64-compiler.exe" \
  --gui-exe "$PWD/build/windows/MinionRush Builder.exe" \
  --template "$PWD/build/ios-generic/MinionRush-generic-template-unsigned.zip" \
  --output "$PWD/build/ios-generic/windows-kit-1.2" \
  --mode release

python3 tools/build_windows_onefile.py \
  --launcher "$PWD/build/windows/MinionRush Portable.exe" \
  --kit "$PWD/build/ios-generic/windows-kit-1.2" \
  --output "$PWD/build/ios-generic/MinionRush-Builder-Windows-1.2.3.exe"
```

Outputs are created only in new, nonexistent paths. The normal release
profile does not contain UTM-specific transport. The separate `utm-test`
build profile remains only for diagnosing an owner's private virtual-machine
shared drive and is not a general Windows release target.

No private `--hungarian-xlf` option is used for the portable public
candidate. Do not package game text, game-derived profile values, original
ZIPs, signed IPAs or device credentials. The builder UI itself includes
its original bilingual labels.

## Verification and limitations

The release kit verifies the checksums of its native Win32 application,
AOT compiler, embedded Python interpreter, generic iOS template and
source-controlled runtime. The onefile launcher checks the embedded ZIP
before extracting it. The unsigned iOS template requires one thin ARM64
Mach-O executable with an RX `__MRCODE/__text` code slot; the IPA assembler
rejects unexpected assets and signature metadata. SHA-256 verifies the
generated v3 profile and patched ARM64 code. Native output is assembled
atomically. The VM-only network import is distinct from the standard
filesystem import path.

The generic v3 Mac and Windows portable Builders both passed native
original-ZIP-to-unsigned-IPA tests. On Windows 11 ARM in UTM, the x64
self-extracting EXE passed its embedded-kit integrity and self-test, started
a responsive native GUI in the interactive desktop session, and built an
unsigned IPA through Create IPA using the user's locally staged original
ZIP. The checked 1.2.0 GUI result contains 1,369 entries, 942,744 AOT
blocks and no bundled Hungarian game translation. The generated file
passes an independent full ZIP integrity and profile parse check.
Version 1.2.1 corrected the missing iOS startup profile loader. Version
1.2.2 subsequently passed the Windows GUI-to-IPA test. The fixed runtime
rendered on the iOS 18.6 and 27.0 Simulators.

Windows' mapped UTM WebDAV drive cannot directly read this 454 MB ZIP.
For this private UTM test, the ZIP was first copied through UTM's
localhost-only WebDAV server onto the guest's local disk and checked using
its original SHA-256. This is a VM transport limitation, not a requirement
for users with ordinary local ZIP files.

The shared IPA writer uses ZIP deflate level 3 to balance the large native
payload size with short host-side assembly time. On the test source, the
Windows IPA shrank from 447,936,121 to 367,832,694 bytes after this change.

The owner confirmed 1.2.1 game display/running on their physical
iPhone after building and signing the IPA on Windows. The 1.2.2 Windows
Builder was then tested again on the UTM Windows 11 ARM guest: exact
portable EXE self-test passed, and its normal Win32 GUI generated a
367,839,044-byte native unsigned IPA through Create IPA using the
owner's local source ZIP. Independent Windows ZipArchive and v3 profile
header checks passed (1,369 entries; 109 bindings; 942,744 AOT blocks;
164,353,244 bytes of ARM64 code). The owner separately confirmed the
1.2.2 loading overlay and running gameplay on a physical iPhone; audio
and save continuity require further verification. The 1.2.3 Windows EXE
also passed its SHA-256 self-test and native GUI ZIP-to-IPA conversion,
producing the same 1,369-entry, 367,839,044-byte IPA on the Windows VM.
The packaged 1.2.3 license-only update does not alter the iOS runtime.

The Windows candidate is not Authenticode signed. Apple-side code signing
or sideloading requires the user's own account; the Builder does not sign.
The iOS deployment minimum remains 17.0, but supported-range build checks
are not a substitute for actual physical-device tests.

Legal restrictions on the owner's source game and the generated IPA remain
distinct from the distribution of the generic Builder.
See [RELEASE_SCOPE.md](RELEASE_SCOPE.md) and
[INSTALLER_WINDOWS.md](INSTALLER_WINDOWS.md).
