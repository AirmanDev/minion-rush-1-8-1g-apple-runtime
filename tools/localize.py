#!/usr/bin/env python3
"""Create, validate, and compile optional XLIFF 2.1 language packs."""

from __future__ import annotations

import argparse
import json
import re
import shutil
import struct
import sys
import unicodedata
import xml.etree.ElementTree as ET
import zipfile
from collections import Counter
from dataclasses import dataclass
from pathlib import Path

sys.dont_write_bytecode = True

from common import load_json, load_json_object

XLIFF_NAMESPACE = "urn:oasis:names:tc:xliff:document:2.0"
XML_NAMESPACE = "http://www.w3.org/XML/1998/namespace"
ET.register_namespace("", XLIFF_NAMESPACE)

CODE_PATTERN = re.compile(r"^[a-z]{2,3}(?:-[A-Z][a-z]{3})?(?:-[A-Z]{2}|-[0-9]{3})?$")
ENGINE_CODE_PATTERN = re.compile(r"^[a-z]{2}$")
RESERVED_ENGINE_CODES = {
    "ar", "de", "en", "es", "fr", "id", "it", "ja", "ko", "pt", "ru", "th", "tr",
    "zh", "zt",
}
PRINTF_PATTERN = re.compile(
    r"%(?:[1-9][0-9]*\$)?[-+ #0]*(?:[0-9]+|\*)?(?:\.(?:[0-9]+|\*))?"
    r"(?:hh|h|ll|l|j|z|t|L)?[diuoxXfFeEgGaAcspn%@HMS]"
)
COLOR_PATTERN = re.compile(r"#(?:F00|F14|[0-9A-Fa-f]{6}|[0-9A-Fa-f]{3})")
TAG_PATTERN = re.compile(r"</?[A-Za-z][^>]*>")
BRACE_PATTERN = re.compile(r"\{[^{}\r\n]+\}")
LETTER_RUN_PATTERN = re.compile(r"([^\W\d_])\1{7}", re.IGNORECASE)
FORMAT_KEYS = (
    "currency", "quantity", "date", "time", "interval", "interval_day",
    "interval_days", "percentage", "crono", "capital", "custom_time",
    "period_seconds", "period_minutes", "period_hours", "period_days",
)
FLAG_LAYOUTS = {
    "horizontal": "MR_FLAG_BANDS_HORIZONTAL",
    "vertical": "MR_FLAG_BANDS_VERTICAL",
}
FLAG_COLOR_LIMIT = 16
FLAG_SLICE_COUNT = 13


@dataclass(frozen=True)
class TextEntry:
    key: str
    value: str


@dataclass(frozen=True)
class Catalog:
    code: str
    display_name: str
    engine_code: str
    country: str
    flag_source: str
    flag_layout: str
    flag_colors: tuple[int, ...]
    font_archive: str
    font_member: str
    formatting: dict[str, str]
    path: Path


def read_texts(path: Path) -> list[TextEntry]:
    data = path.read_bytes()
    if len(data) < 8:
        raise ValueError(f"truncated text table: {path}")
    count, values_offset = struct.unpack_from("<II", data)
    if values_offset < 8 or values_offset > len(data):
        raise ValueError(f"invalid value-table offset: {path}")
    position = 8
    index: list[tuple[str, int]] = []
    for row in range(count):
        if position + 4 > values_offset:
            raise ValueError(f"truncated text index at row {row}: {path}")
        key_size = struct.unpack_from("<I", data, position)[0]
        position += 4
        if position + key_size + 4 > values_offset:
            raise ValueError(f"invalid key size at row {row}: {path}")
        try:
            key = data[position:position + key_size].decode("utf-8")
        except UnicodeDecodeError as exc:
            raise ValueError(f"invalid UTF-8 key at row {row}: {path}") from exc
        position += key_size
        value_offset = struct.unpack_from("<I", data, position)[0]
        position += 4
        index.append((key, value_offset))
    if position != values_offset:
        raise ValueError(f"text index has trailing bytes: {path}")

    entries: list[TextEntry] = []
    expected_offset = 0
    for row, (key, value_offset) in enumerate(index):
        if value_offset != expected_offset or values_offset + value_offset + 4 > len(data):
            raise ValueError(f"noncanonical value offset at row {row}: {path}")
        value_size = struct.unpack_from("<I", data, values_offset + value_offset)[0]
        start = values_offset + value_offset + 4
        end = start + value_size
        if end > len(data):
            raise ValueError(f"invalid value size at row {row}: {path}")
        try:
            value = data[start:end].decode("utf-8")
        except UnicodeDecodeError as exc:
            raise ValueError(f"invalid UTF-8 value at row {row}: {path}") from exc
        entries.append(TextEntry(key, value))
        expected_offset += 4 + value_size
    if values_offset + expected_offset != len(data):
        raise ValueError(f"text table has trailing bytes: {path}")
    return entries


def encode_texts(entries: list[TextEntry]) -> bytes:
    index = bytearray()
    values = bytearray()
    for entry in entries:
        key = entry.key.encode("utf-8")
        value = entry.value.encode("utf-8")
        index += struct.pack("<I", len(key)) + key + struct.pack("<I", len(values))
        values += struct.pack("<I", len(value)) + value
    return struct.pack("<II", len(entries), 8 + len(index)) + index + values


def parse_color(value: object, label: str) -> int:
    if not isinstance(value, str) or not re.fullmatch(r"[0-9A-F]{6}", value):
        raise ValueError(f"{label} must be a six-digit uppercase RGB value")
    return int(value, 16)


def load_catalog(path: Path, root: Path) -> Catalog:
    value = load_json_object(path)
    code = value.get("code")
    name = value.get("display_name")
    engine_code = value.get("engine_code")
    country = value.get("country")
    if not isinstance(code, str) or not CODE_PATTERN.fullmatch(code):
        raise ValueError(f"invalid BCP 47 language code: {path}")
    if not isinstance(name, str) or not name.strip() or unicodedata.normalize("NFC", name) != name:
        raise ValueError(f"invalid native display name: {path}")
    if not isinstance(engine_code, str) or not ENGINE_CODE_PATTERN.fullmatch(engine_code):
        raise ValueError(f"invalid engine language code: {path}")
    if engine_code in RESERVED_ENGINE_CODES:
        raise ValueError(f"engine language code is reserved by the original game: {path}")
    if not isinstance(country, str) or not re.fullmatch(r"[A-Z]{2}", country):
        raise ValueError(f"invalid country code: {path}")

    flag = value.get("flag")
    font = value.get("font")
    formatting = value.get("format")
    if not isinstance(flag, dict) or not isinstance(font, dict) or not isinstance(formatting, dict):
        raise ValueError(f"incomplete localization configuration: {path}")
    flag_source = flag.get("source")
    flag_layout = flag.get("layout")
    colors = flag.get("colors")
    if not isinstance(flag_source, str) or not (root / flag_source).is_file():
        raise ValueError(f"flag source does not exist: {path}")
    if flag_layout not in FLAG_LAYOUTS:
        raise ValueError(f"flag layout must be horizontal or vertical: {path}")
    if not isinstance(colors, list) or not 1 <= len(colors) <= FLAG_COLOR_LIMIT:
        raise ValueError(f"flag must define between 1 and {FLAG_COLOR_LIMIT} colors: {path}")
    parsed_colors = tuple(parse_color(color, f"flag color in {path}") for color in colors)
    flag_text = (root / flag_source).read_text(encoding="utf-8")
    source_colors = {
        value.upper() if len(value) == 6 else "".join(character * 2 for character in value).upper()
        for value in re.findall(r"#([0-9A-Fa-f]{6}|[0-9A-Fa-f]{3})", flag_text)
    }
    if any(color not in source_colors for color in colors):
        raise ValueError(f"flag colors do not match the declared source: {path}")
    archive = font.get("archive")
    member = font.get("member")
    if not isinstance(archive, str) or not isinstance(member, str) or not member:
        raise ValueError(f"invalid font source: {path}")
    if set(formatting) != set(FORMAT_KEYS) or not all(
        isinstance(formatting[key], str) and formatting[key] for key in FORMAT_KEYS
    ):
        raise ValueError(f"invalid formatter configuration: {path}")
    return Catalog(
        code, name, engine_code, country, flag_source, flag_layout, parsed_colors, archive, member,
        {key: formatting[key] for key in FORMAT_KEYS}, path,
    )


def load_catalogs(root: Path) -> list[Catalog]:
    directory = root / "config/localizations"
    catalogs = [load_catalog(path, root) for path in sorted(directory.glob("*.json"))]
    codes = [catalog.code.casefold() for catalog in catalogs]
    if len(codes) != len(set(codes)):
        raise ValueError("duplicate localization code")
    engine_codes = [catalog.engine_code.casefold() for catalog in catalogs]
    if len(engine_codes) != len(set(engine_codes)):
        raise ValueError("duplicate engine language code")
    return catalogs


def token_signature(value: str) -> Counter[str]:
    tokens = PRINTF_PATTERN.findall(value)
    tokens += COLOR_PATTERN.findall(value)
    tokens += TAG_PATTERN.findall(value)
    tokens += BRACE_PATTERN.findall(value)
    return Counter(tokens)


def create_xliff(source: list[TextEntry], code: str, destination: Path) -> None:
    if destination.exists():
        raise ValueError(f"translation already exists: {destination}")
    root = ET.Element(f"{{{XLIFF_NAMESPACE}}}xliff", {"version": "2.1", "srcLang": "en", "trgLang": code})
    file_node = ET.SubElement(root, f"{{{XLIFF_NAMESPACE}}}file", {"id": "game", "original": "text/en.texts"})
    for index, entry in enumerate(source):
        unit = ET.SubElement(file_node, f"{{{XLIFF_NAMESPACE}}}unit", {"id": str(index), "name": entry.key})
        segment = ET.SubElement(unit, f"{{{XLIFF_NAMESPACE}}}segment", {"state": "initial"})
        source_node = ET.SubElement(segment, f"{{{XLIFF_NAMESPACE}}}source", {f"{{{XML_NAMESPACE}}}space": "preserve"})
        source_node.text = entry.value
        target = ET.SubElement(segment, f"{{{XLIFF_NAMESPACE}}}target", {f"{{{XML_NAMESPACE}}}space": "preserve"})
        target.text = ""
    ET.indent(root, space="  ")
    destination.parent.mkdir(parents=True, exist_ok=True)
    ET.ElementTree(root).write(destination, encoding="utf-8", xml_declaration=True, short_empty_elements=False)


def read_xliff(path: Path, source: list[TextEntry], code: str) -> list[TextEntry]:
    try:
        root = ET.parse(path).getroot()
    except ET.ParseError as exc:
        raise ValueError(f"invalid XLIFF: {path}: {exc}") from exc
    namespace = {"x": XLIFF_NAMESPACE}
    if root.tag != f"{{{XLIFF_NAMESPACE}}}xliff" or root.get("version") != "2.1":
        raise ValueError(f"XLIFF 2.1 is required: {path}")
    if root.get("srcLang") != "en" or root.get("trgLang") != code:
        raise ValueError(f"XLIFF language metadata does not match {code}: {path}")
    units = root.findall("x:file/x:unit", namespace)
    if len(units) != len(source):
        raise ValueError(f"XLIFF has {len(units)} units; expected {len(source)}: {path}")
    translated: list[TextEntry] = []
    for index, (unit, original) in enumerate(zip(units, source)):
        if unit.get("id") != str(index) or unit.get("name") != original.key:
            raise ValueError(f"XLIFF key order differs at unit {index}: {path}")
        segment = unit.find("x:segment", namespace)
        source_node = unit.find("x:segment/x:source", namespace)
        target_node = unit.find("x:segment/x:target", namespace)
        source_value = "" if source_node is None or source_node.text is None else source_node.text
        target_value = "" if target_node is None or target_node.text is None else target_node.text
        if source_value != original.value:
            raise ValueError(f"XLIFF source differs at unit {index}: {path}")
        if not target_value or (original.value.strip() and not target_value.strip()):
            raise ValueError(f"empty translation at unit {index}: {path}")
        if segment is None or segment.get("state") != "final":
            raise ValueError(f"translation is not final at unit {index}: {path}")
        if "\0" in target_value or unicodedata.normalize("NFC", target_value) != target_value:
            raise ValueError(f"translation is not canonical Unicode at unit {index}: {path}")
        if LETTER_RUN_PATTERN.search(target_value):
            raise ValueError(f"translation contains a repeated-letter run at unit {index}: {path}")
        if len(target_value) > max(24, len(original.value) * 4):
            raise ValueError(f"translation expands excessively at unit {index}: {path}")
        if token_signature(target_value) != token_signature(original.value):
            raise ValueError(f"placeholder mismatch at unit {index}: {path}")
        translated.append(TextEntry(original.key, target_value))
    return translated


def write_if_changed(path: Path, data: bytes) -> None:
    if path.is_file() and path.read_bytes() == data:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_bytes(data)
    temporary.replace(path)


def formatter_data(catalogs: list[Catalog], source: Path) -> bytes:
    rows = load_json(source)
    if not isinstance(rows, list) or not all(isinstance(row, dict) for row in rows):
        raise ValueError(f"invalid formatter table: {source}")
    existing = {str(row.get("code", "")).casefold() for row in rows}
    for catalog in catalogs:
        if catalog.engine_code.casefold() in existing:
            raise ValueError(
                f"engine language code already exists in formatter table: {catalog.engine_code}"
            )
        rows.append({"code": catalog.engine_code, **catalog.formatting})
        existing.add(catalog.engine_code.casefold())
    return (json.dumps(rows, ensure_ascii=False, indent=2) + "\n").encode("utf-8")


def read_binary_string(data: bytes, position: int, source: Path) -> tuple[str, int]:
    if position + 2 > len(data):
        raise ValueError(f"truncated font table: {source}")
    size = struct.unpack_from("<H", data, position)[0]
    position += 2
    end = position + size
    if end > len(data):
        raise ValueError(f"invalid font-table string: {source}")
    try:
        return data[position:end].decode("utf-8"), end
    except UnicodeDecodeError as exc:
        raise ValueError(f"invalid UTF-8 in font table: {source}") from exc


def write_binary_string(value: str) -> bytes:
    data = value.encode("utf-8")
    if len(data) > 0xffff:
        raise ValueError("font-table string is too long")
    return struct.pack("<H", len(data)) + data


def localized_font_info(catalogs: list[Catalog], source: Path) -> bytes:
    data = source.read_bytes()
    if len(data) < 4:
        raise ValueError(f"truncated font table: {source}")
    style_count = struct.unpack_from("<I", data)[0]
    position = 4
    styles: list[tuple[str, tuple[str, int, int], list[tuple[str, int, int, list[str]]]]] = []
    for _ in range(style_count):
        style_name, position = read_binary_string(data, position, source)
        default_name, position = read_binary_string(data, position, source)
        if position + 12 > len(data):
            raise ValueError(f"truncated font style: {source}")
        default_size, default_flags, alternative_count = struct.unpack_from(
            "<III", data, position
        )
        position += 12
        alternatives: list[tuple[str, int, int, list[str]]] = []
        for _ in range(alternative_count):
            font_name, position = read_binary_string(data, position, source)
            if position + 12 > len(data):
                raise ValueError(f"truncated alternate font: {source}")
            size, flags, language_count = struct.unpack_from("<III", data, position)
            position += 12
            languages: list[str] = []
            for _ in range(language_count):
                language, position = read_binary_string(data, position, source)
                languages.append(language)
            configured_codes = {catalog.engine_code.casefold() for catalog in catalogs}
            duplicate = next(
                (language for language in languages if language.casefold() in configured_codes),
                None,
            )
            if duplicate is not None:
                raise ValueError(f"engine language code already exists in font table: {duplicate}")
            alternatives.append((font_name, size, flags, languages))
        for catalog in catalogs:
            source_font = Path(catalog.font_member).name.casefold()
            match = next(
                (index for index, alternative in enumerate(alternatives)
                 if alternative[0].casefold() == source_font),
                None,
            )
            if match is None:
                raise ValueError(
                    f"configured localization font is absent from {source}: {source_font}"
                )
            font_name, size, flags, languages = alternatives[match]
            alternatives[match] = (
                font_name,
                size,
                flags,
                [*languages, catalog.engine_code.upper()],
            )
        styles.append((style_name, (default_name, default_size, default_flags), alternatives))
    if position != len(data):
        raise ValueError(f"font table has trailing bytes: {source}")

    output = bytearray(struct.pack("<I", len(styles)))
    for style_name, default, alternatives in styles:
        output += write_binary_string(style_name)
        output += write_binary_string(default[0])
        output += struct.pack("<III", default[1], default[2], len(alternatives))
        for font_name, size, flags, languages in alternatives:
            output += write_binary_string(font_name)
            output += struct.pack("<III", size, flags, len(languages))
            for language in languages:
                output += write_binary_string(language)
    return bytes(output)


def font_codepoints(data: bytes, label: str) -> set[int]:
    if len(data) < 12:
        raise ValueError(f"invalid TrueType/OpenType font: {label}")
    table_count = struct.unpack_from(">H", data, 4)[0]
    tables: dict[bytes, tuple[int, int]] = {}
    for index in range(table_count):
        position = 12 + index * 16
        if position + 16 > len(data):
            raise ValueError(f"truncated TrueType/OpenType directory: {label}")
        tag, _, offset, size = struct.unpack_from(">4sIII", data, position)
        if offset + size > len(data):
            raise ValueError(f"invalid TrueType/OpenType table: {label}")
        tables[tag] = (offset, size)
    if b"cmap" not in tables:
        raise ValueError(f"font has no Unicode character map: {label}")
    cmap, cmap_size = tables[b"cmap"]
    if cmap_size < 4:
        raise ValueError(f"invalid font character map: {label}")
    cmap_end = cmap + cmap_size
    subtable_count = struct.unpack_from(">H", data, cmap + 2)[0]
    if cmap + 4 + subtable_count * 8 > cmap_end:
        raise ValueError(f"truncated font character-map directory: {label}")
    result: set[int] = set()
    for index in range(subtable_count):
        record = cmap + 4 + index * 8
        platform, encoding, relative = struct.unpack_from(">HHI", data, record)
        if platform != 0 and not (platform == 3 and encoding in (1, 10)):
            continue
        subtable = cmap + relative
        if subtable + 2 > cmap_end:
            raise ValueError(f"invalid font character-map offset: {label}")
        format_id = struct.unpack_from(">H", data, subtable)[0]
        if format_id == 4:
            if subtable + 14 > cmap_end:
                raise ValueError(f"truncated format 4 character map: {label}")
            length = struct.unpack_from(">H", data, subtable + 2)[0]
            segment_count = struct.unpack_from(">H", data, subtable + 6)[0] // 2
            subtable_end = subtable + length
            if length < 16 or subtable_end > cmap_end or segment_count == 0:
                raise ValueError(f"invalid format 4 character map: {label}")
            ends = subtable + 14
            starts = ends + segment_count * 2 + 2
            deltas = starts + segment_count * 2
            range_offsets = deltas + segment_count * 2
            if range_offsets + segment_count * 2 > subtable_end:
                raise ValueError(f"truncated format 4 segments: {label}")
            for segment in range(segment_count):
                end = struct.unpack_from(">H", data, ends + segment * 2)[0]
                start = struct.unpack_from(">H", data, starts + segment * 2)[0]
                delta = struct.unpack_from(">H", data, deltas + segment * 2)[0]
                offset_position = range_offsets + segment * 2
                range_offset = struct.unpack_from(">H", data, offset_position)[0]
                if start > end:
                    raise ValueError(f"invalid format 4 segment: {label}")
                for codepoint in range(start, end + 1):
                    if codepoint == 0xffff:
                        continue
                    if range_offset == 0:
                        glyph = (codepoint + delta) & 0xffff
                    else:
                        glyph_position = offset_position + range_offset + 2 * (codepoint - start)
                        if glyph_position + 2 > subtable_end:
                            raise ValueError(f"invalid format 4 glyph offset: {label}")
                        glyph = struct.unpack_from(">H", data, glyph_position)[0]
                        if glyph:
                            glyph = (glyph + delta) & 0xffff
                    if glyph:
                        result.add(codepoint)
        elif format_id == 12:
            if subtable + 16 > cmap_end:
                raise ValueError(f"truncated format 12 character map: {label}")
            length = struct.unpack_from(">I", data, subtable + 4)[0]
            group_count = struct.unpack_from(">I", data, subtable + 12)[0]
            subtable_end = subtable + length
            if (length < 16 or subtable_end > cmap_end or
                    subtable + 16 + group_count * 12 > subtable_end):
                raise ValueError(f"invalid format 12 character map: {label}")
            for group in range(group_count):
                start, end, glyph = struct.unpack_from(">III", data, subtable + 16 + group * 12)
                if start > end or end > 0x10ffff:
                    raise ValueError(f"invalid format 12 group: {label}")
                if glyph == 0:
                    start += 1
                if start <= end:
                    result.update(range(start, end + 1))
    return result


def extract_font(catalog: Catalog, assets: Path, entries: list[TextEntry]) -> bytes:
    archive_path = assets / catalog.font_archive
    if not archive_path.is_file():
        raise ValueError(f"font archive does not exist: {archive_path}")
    try:
        with zipfile.ZipFile(archive_path) as archive:
            data = archive.read(catalog.font_member)
    except (zipfile.BadZipFile, KeyError) as exc:
        raise ValueError(f"font member does not exist: {archive_path}:{catalog.font_member}") from exc
    if len(data) < 12 or data[:4] not in (b"\x00\x01\x00\x00", b"OTTO", b"true"):
        raise ValueError(f"invalid TrueType/OpenType font: {archive_path}:{catalog.font_member}")
    required = {
        ord(character)
        for entry in entries
        for character in entry.value
        if ord(character) >= 0x80 and unicodedata.category(character)[0] in ("L", "M")
    }
    missing = required - font_codepoints(data, f"{archive_path}:{catalog.font_member}")
    if missing:
        characters = "".join(chr(codepoint) for codepoint in sorted(missing))
        raise ValueError(f"localization font lacks translated letters {characters!r}: {archive_path}")
    return data


def c_string(value: str) -> str:
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def generated_header(enabled: list[Catalog]) -> bytes:
    rectangle_capacity = sum(
        FLAG_SLICE_COUNT * (1 + len(catalog.flag_colors)) for catalog in enabled
    )
    lines = [
        "#ifndef MR_LOCALIZATION_CONFIG_H",
        "#define MR_LOCALIZATION_CONFIG_H",
        "",
        f"#define MR_LOCALIZATION_CONFIG_COUNT {len(enabled)}u",
        f"#define MR_LOCALIZATION_FLAG_SLICE_COUNT {FLAG_SLICE_COUNT}u",
        f"#define MR_LOCALIZATION_FLAG_RECTANGLE_CAPACITY {rectangle_capacity}u",
    ]
    for index, catalog in enumerate(enabled):
        colors = ", ".join(f"0x{color:06X}u" for color in catalog.flag_colors)
        lines.append(
            f"static const uint32_t MR_LOCALIZATION_FLAG_COLORS_{index}[] = {{{colors}}};"
        )
    if enabled:
        lines.append("")
    lines += [
        "static const mr_localization_definition MR_LOCALIZATION_CONFIGS[] = {",
    ]
    if enabled:
        for index, catalog in enumerate(enabled):
            lines.append(
                "    {" + ", ".join((c_string(catalog.code), c_string(catalog.display_name),
                 c_string(catalog.engine_code), c_string(catalog.country),
                 FLAG_LAYOUTS[catalog.flag_layout], f"MR_LOCALIZATION_FLAG_COLORS_{index}",
                 f"{len(catalog.flag_colors)}u")) + "},"
            )
    else:
        lines.append("    {0},")
    lines += ["};", "", "#endif", ""]
    return "\n".join(lines).encode("utf-8")


def remove_generated_outputs(root: Path, assets: Path) -> None:
    text_directory = assets / "game/files/text"
    for catalog in load_catalogs(root):
        generated_text = text_directory / f"{catalog.engine_code}.texts"
        if not generated_text.exists():
            continue
        if generated_text.is_symlink() or not generated_text.is_file():
            raise ValueError(f"invalid generated localization text: {generated_text}")
        generated_text.unlink()

    output_root = assets / "localizations"
    if output_root.exists():
        if output_root.is_symlink() or not output_root.is_dir():
            raise ValueError(f"invalid generated localization directory: {output_root}")
        shutil.rmtree(output_root)


def prepare(root: Path, assets: Path, header: Path) -> list[Catalog]:
    source_path = assets / "game/files/text/en.texts"
    source = read_texts(source_path)
    formatter = assets / "game/files/localizationText/langs.json"
    font_info = assets / "game/files/datalibs/gui_fonts_info"
    output_root = assets / "localizations"
    catalogs = load_catalogs(root)
    remove_generated_outputs(root, assets)
    enabled: list[Catalog] = []
    translations: dict[str, list[TextEntry]] = {}
    for catalog in catalogs:
        translation = root / "localizations" / f"{catalog.code}.xlf"
        if not translation.is_file():
            continue
        entries = read_xliff(translation, source, catalog.code)
        extract_font(catalog, assets, entries)
        translations[catalog.code] = entries
        enabled.append(catalog)

    for catalog in enabled:
        entries = translations[catalog.code]
        write_if_changed(source_path.parent / f"{catalog.engine_code}.texts", encode_texts(entries))
    if enabled:
        runtime = output_root / "runtime"
        if runtime.exists():
            if runtime.is_symlink() or not runtime.is_dir():
                raise ValueError(f"invalid generated localization directory: {runtime}")
            shutil.rmtree(runtime)
        write_if_changed(runtime / "localizationText/langs.json", formatter_data(enabled, formatter))
        write_if_changed(runtime / "datalibs/gui_fonts_info", localized_font_info(enabled, font_info))
    write_if_changed(header, generated_header(enabled))
    return enabled


def command_init(args: argparse.Namespace) -> None:
    root = Path(args.root).resolve()
    assets = (root / args.assets_root).resolve()
    catalog = next((item for item in load_catalogs(root) if item.code == args.code), None)
    if catalog is None:
        raise ValueError(f"unknown localization configuration: {args.code}")
    destination = root / "localizations" / f"{catalog.code}.xlf"
    create_xliff(read_texts(assets / "game/files/text/en.texts"), catalog.code, destination)
    print(f"created {destination.relative_to(root)}")


def command_prepare(args: argparse.Namespace) -> None:
    root = Path(args.root).resolve()
    assets = (root / args.assets_root).resolve()
    enabled = prepare(root, assets, (root / args.header).resolve())
    names = ", ".join(catalog.code for catalog in enabled) or "none"
    print(f"localizations: {names}")


def command_clean(args: argparse.Namespace) -> None:
    root = Path(args.root).resolve()
    remove_generated_outputs(root, (root / args.assets_root).resolve())
    print("localization outputs removed")


def command_validate(args: argparse.Namespace) -> None:
    root = Path(args.root).resolve()
    assets = (root / args.assets_root).resolve()
    source = read_texts(assets / "game/files/text/en.texts")
    formatter = assets / "game/files/localizationText/langs.json"
    font_info = assets / "game/files/datalibs/gui_fonts_info"
    catalogs = load_catalogs(root)
    enabled: list[Catalog] = []
    for catalog in catalogs:
        translation = root / "localizations" / f"{catalog.code}.xlf"
        if translation.is_file():
            entries = read_xliff(translation, source, catalog.code)
            extract_font(catalog, assets, entries)
            enabled.append(catalog)
    formatter_data(enabled, formatter)
    localized_font_info(enabled, font_info)
    print(f"localization validation: passed ({len(catalogs)} configured)")


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--root", default=".")
    result.add_argument("--assets-root", default="assets")
    subparsers = result.add_subparsers(dest="command", required=True)
    init = subparsers.add_parser("init", help="create an untranslated private XLIFF catalog")
    init.add_argument("code")
    init.set_defaults(function=command_init)
    prepare_parser = subparsers.add_parser("prepare", help="compile every available language pack")
    prepare_parser.add_argument("--header", default="build/localization_config.h")
    prepare_parser.set_defaults(function=command_prepare)
    clean_parser = subparsers.add_parser("clean", help="remove generated localization outputs")
    clean_parser.set_defaults(function=command_clean)
    validate_parser = subparsers.add_parser("validate", help="validate configuration and available XLIFF files")
    validate_parser.set_defaults(function=command_validate)
    return result


def main() -> int:
    args = parser().parse_args()
    try:
        args.function(args)
    except (OSError, ValueError, zipfile.BadZipFile) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
