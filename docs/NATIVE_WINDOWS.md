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
  --output "$PWD/build/ios-generic/MinionRush-Builder-Windows-1.2.8.exe"
```

Outputs are created only in new, nonexistent paths. The normal release
profile does not contain UTM-specific transport. The separate `utm-test`
build profile remains only for diagnosing an owner's private virtual-machine
shared drive and is not a general Windows release target.

No private `--hungarian-xlf` option is used for the portable public
Builder. Do not package game text, game-derived profile values, original
ZIPs, signed IPAs or device credentials. The builder UI itself includes
its original bilingual labels.

## Verification and limitations

The release kit validates the native Win32 executable, host AOT compiler,
embedded Python distribution, generic iOS template, pinned runtime source
and applicable license files. The onefile launcher checks its embedded ZIP
before unpacking. The IPA assembly stage validates the native Mach-O
code slot, game profile and output archive. A failed or cancelled build
does not intentionally destroy a previously completed IPA.

The Mac and Windows Builders were tested with an independently supplied
original 1.8.1g ZIP. The Windows 11 ARM UTM VM executed the x64 Builder
in its native GUI and passed the complete self-test and IPA creation flow.
The owner also confirmed on-device installation, gameplay, audio,
Hungarian text and save continuity from privately built applications.
These tests do not establish universal device compatibility.

The public Windows Builder is not Authenticode signed. Device-side
installation and iOS signing require the user's own appropriate Apple
account and credentials via a separate signing utility. The minimum
deployment target is iOS/iPadOS 17; building against a platform version
does not certify runtime compatibility with all devices.

The user owns responsibility for lawful game acquisition and any rights
associated with the locally generated IPA. This is an independent,
unofficial compatibility tool. See [distribution scope](RELEASE_SCOPE.md)
and [Windows usage](INSTALLER_WINDOWS.md).
