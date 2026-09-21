# Localization

Community language packs use XLIFF 2.1 as their editable source format and a
deterministic compiler for the game's ordered binary text table. The original
and translated game text is deliberately excluded from Git. This keeps the
source repository distributable without publishing proprietary game content.

Each supported community locale has a small public configuration in
`config/localizations/`. It declares a BCP 47 language tag, native display name,
formatter rules, a redistributable flag source, a horizontal or vertical flag
band layout, and the font already present in the user's installed game assets.
Runtime code discovers only packs whose XLIFF file is available locally.

To start a configured language pack after installing the game assets:

```sh
python3 tools/localize.py init hu
```

Translate every `target` element in `localizations/hu.xlf`, preserve all format
placeholders exactly, mark reviewed segments as `final`, and then run
`./build.sh`. The build validates the source
text, key order, Unicode NFC normalization, placeholders, locale metadata, font,
and formatter data before it writes the ignored engine table to
`assets/game/files/text/<engine-code>.texts` and shared runtime metadata under
`assets/localizations/runtime/`.

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

Adding another language requires one configuration JSON file, a flag source
with clear redistribution terms, and its local XLIFF file. `flag.layout` is
`horizontal` or `vertical`; `flag.colors` lists 1 to 16 equal-sized bands in
display order. No runtime source branch, position constant, or duplicated
button implementation is needed. The engine pages receive all packs compiled
for that installation, extend and center rows as necessary, and pass each
pack's two-letter code through the existing language-selection flow.

The Hungarian flag in `resources/flags/hu.svg` comes from Wikimedia Commons,
where the simple national flag is marked public domain. Its source page is
<https://commons.wikimedia.org/wiki/File:Flag_of_Hungary.svg>.
