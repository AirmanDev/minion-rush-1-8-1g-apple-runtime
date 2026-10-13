# Localization

Community translations use XLIFF 2.1 for editing and a versioned target-only
JSON pack for desktop distribution. The public pack contains only the ordered
translated targets, the language tag and a SHA-256 fingerprint of the original
English text table. No original English source text, original game asset,
binary or private editable XLIFF is distributed in the Builder.

The Hungarian pack is at `language_packs/hu.json` and contains 2,483 targets.
Both portable Builders include it as an optional additional language by
default. The game keeps all original languages from the user's own 1.8.1g
archive. The Hungarian table is produced once during IPA creation at the
user's computer, alongside localized formatter/font metadata. It is saved
under `game/files/text/hu.texts` in the private IPA. Nothing rewrites
existing language tables during play.

The compiler checks the original source table SHA-256, translation entry
count, Unicode NFC, formatting placeholders and available font glyphs before
building a local IPA. A mismatched original archive or changed translation
is rejected. The Builder does not fetch a game or translation over the
network. The desktop English/Magyar interface switch is independent of the
in-game language choice.

## Maintaining and adding packs

Each supported language has a configuration in `config/localizations/`.
The configuration specifies its BCP 47 language tag, native name, distinct
two-letter engine code, flag source, color bands, font and formatting data.
The engine's localization and language-button layout are data-driven and
handle multiple configured community packs without language-specific branches.

For a new language, add its configuration and redistributable flag asset.
Create its editable XLIFF from your locally installed original assets:

```sh
python3 tools/localize.py init xx
```

Translate the target elements, keep source and placeholder order unchanged,
mark segments final, then export the public, source-stripped translation pack:

```sh
python3 tools/localize.py export-pack xx
python3 tools/localize.py validate
```

The `xx` code is a placeholder for the configured BCP 47 code. Inspect the
generated `language_packs/xx.json` and confirm you can redistribute the
translation before adding it to the public source manifest. Rebuild the
generic iOS template and the desktop Builders after adding or changing
language configurations so the in-game button definitions agree with the
packaged catalogs. The runtime automatically discovers the enabled packs and
adds them to the existing language menu. The desktop toggle currently
controls the available additional-language set as a group. New languages
are not silently enabled in existing installed IPAs; the user must create
a new IPA with the selected language packs.

Developers can still use private `localizations/<code>.xlf` catalogs during
local builds. A private XLIFF and public JSON for the same language in one
build input directory are rejected as ambiguous. The public Builder does
not include editable XLIFF content.

The user's game selects among packaged languages through its original
language-selection mechanism. Its preference persists across relaunches;
no online translation or periodic text replacement occurs. At startup a
small compatibility hook registers community languages after the game's
localization singleton is ready. This changes the available menu choices,
not existing game text files.

The translation itself may have copyright or derivative-work restrictions.
Removing English source strings reduces distribution of original material
but does not constitute permission from the game's rightsholders.

The compiler creates one shared runtime formatter and font table for all
installed community packs. These files are loaded before any language switch,
so the engine can validate a community font even when its current locale is one
of the built-in languages. The compiler adds each engine code to the configured
font alternative already present in the installed game data; it does not copy
or rename the font. Compilation rejects a pack when that font lacks any
non-ASCII letter used by the translation.

The original executable has a fixed list of language buttons. Between engine
frames, the runtime adds each community locale to the page's own localization
map and creates a normal engine `InterfaceButton`. The final partially filled
built-in row and all community buttons are laid out together; every incomplete
row is centered automatically. Buttons that the engine deliberately places
outside the visible grid remain hidden and do not consume community-language
slots. A rounded flag is drawn from the locale's declared bands with engine UI
objects, so it shares the same hierarchy, hit testing, visibility, and lifetime
as the built-in buttons. Community flags use a common inactive tint and show
their full colors with an orange border only when selected. No platform overlay
or separate language menu is used.

During startup, the engine first receives its known English bootstrap code.
After Babel creates its localization singleton on the render thread, the host
selects the configured community code once. This avoids depending on the
engine's closed startup language enumeration while retaining native runtime
language switching.

Flag sources need clear redistribution terms. `flag.layout` is
`horizontal` or `vertical`, and `flag.colors` lists 1 to 16 equal-sized
bands in display order. The game pages extend and center rows as needed and
use the configured two-letter engine codes.

The Hungarian flag in `resources/flags/hu.svg` comes from Wikimedia Commons,
where the simple national flag is marked public domain. Its source page is
<https://commons.wikimedia.org/wiki/File:Flag_of_Hungary.svg>.
