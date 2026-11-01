#!/usr/bin/env python3
"""Generate a synthetic localization test mod for OpenChaos.

The mod exercises every text hook of the localization-mod support without any
real translation: each value is a tagged Cyrillic filler ("T12 проверка ...")
that is at least 40 % longer than its English key, so a translated string is
obvious on screen and the longer-string and CP1251 paths are both used. The tag
("T<row>") is plain ASCII, so it stays readable even before a mod font exists.

No game text is stored in this script or in the repository: the English keys
are read from YOUR copy of the game data at generation time, and the mod is
written into the user data folder (outside the repository).

Rows written (keys taken from the game data):
  lang  main-menu and options labels, plus "Press |16 to continue." style rows
        (keeps the |N token)
  sty   title and briefing of the chosen missions (briefing becomes 2 lines)
  ucm   every dialogue/message record of the chosen levels ('|' separators kept)
  txt   every citizen line of text/citsez.txt and of the levels' own citizen files
  own   strings OpenChaos itself added: the pause menu's Help item, the help topic
        titles, one help line with its {glyph} token, the Options menu's Language
        item and restart note (keys are in OWN_STRINGS below, not game data)

Other files written:
  font2d.tga + font2d.map      FONT2D atlas for bytes 0x80..0xFF
  menufont.tga + menufont.map  menu atlas for bytes 0x80..0xFF and lowercase a..z
  voice/talk2/misc/ROPER-TAUNT03.wav  a 1 s sine, so the overridden bark is audible

The glyphs are the public-domain font8x8 already in the repository, read from
new_game/src/game/missing_resources.cpp. It has no Cyrillic, so each CP1251
letter draws its Latin transliteration and the other high bytes draw the last
hex digit of the byte. Every glyph carries a 2 px bar along its bottom: a letter
with a bar comes from the mod atlas, a letter without one from the game's font.

Usage:
  python make_testmod.py [--game-dir DIR] [--out-root DIR] [--name testmod]
                         [--levels Assault1.ucm,FTutor1.ucm,police1.ucm]
                         [--broken] [--select NAME|""]

  --game-dir  folder with the original game data (text/, data/, levels/).
              Default: <repo>/original_game_resources.
  --out-root  OpenChaos user data folder. Default: the platform's
              (%APPDATA%/OpenChaos, ~/Library/Application Support/OpenChaos,
              ~/.local/share/OpenChaos). The mod goes to
              <out-root>/localizations/<name>/.
  --broken    also write broken sibling mods for the discovery checks:
              <name>_nomanifest, <name>_badjson, <name>_notable, <name>_badtable.
  --select    write "localization": {"mod": NAME} into
              <out-root>/OpenChaos.config.json ("" clears the selection).

The game only reads the mod when it is built with the feature flag
ext_localization_mods = true (new_game/src/feature_flags.cpp).
"""

import argparse
import json
import math
import os
import re
import struct
import sys
import wave
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
XLAT_HEADER = REPO / "new_game" / "src" / "assets" / "xlat_str.h"
FONT8X8_SOURCE = REPO / "new_game" / "src" / "game" / "missing_resources.cpp"

# Lang ids translated by the test mod (main menu, options, |N prompts).
LANG_IDS = [
    "X_START", "X_LOAD_GAME", "X_OPTIONS", "X_CREDITS", "X_EXIT",
    "X_SOUND", "X_VOLUME", "X_AMBIENT", "X_MUSIC",
    "X_PRESS_TO_CONTINUE", "X_ENTER_VEHICLE",
]

DEFAULT_LEVELS = "Assault1.ucm,FTutor1.ucm,police1.ucm"

# Strings OpenChaos itself added. These are not game data, so they are spelled
# out here. The help line has a {glyph} token, which its value has to keep.
OWN_STRINGS = [
    "Help", "CONTROLS", "MOVEMENT", "COMBAT", "WEAPONS", "DRIVING", "OTHER",
    "{kb_space} - Jump.", "Language", "Restart to apply", "Bonus mission unlocked!",
]

# Every value is at least this much longer than its key.
GROWTH = 1.4
# Byte caps that keep every generated value inside the engine's limits:
# one displayed message line, a mission title, and half a briefing.
MESSAGE_LINE_CAP = 240
TITLE_CAP = 200
BRIEF_HALF_CAP = 800

# Filler words. None of them occurs in the English game text.
FILLER = "проверка перевода образец текста строка длиннее английской "

# .ucm header: version, used, then five _MAX_PATH (260) names; the citizen
# text file name is the fifth (see ELEV_load_name in assets/formats/elev.cpp).
UCM_NAME_SIZE = 260
UCM_CITSEZ_OFFSET = 4 + 4 + 4 * UCM_NAME_SIZE
# A message record is: byte 1, int32 length, text (assets/formats/elev.cpp).
UCM_MESSAGE_TYPE = 1
UCM_MESSAGE_MAX = 2047

# The longest mod folder name the game accepts (LOC_NAME_MAX_BYTES in
# new_game/src/localization/loc_config.cpp).
MOD_NAME_MAX_BYTES = 64

# font8x8 covers ASCII from FONT8X8_FIRST_CH; a glyph is 8 rows of 8 pixels.
FONT8X8_FIRST_CH = 0x20
FONT8X8_PX = 8
# Latin transliteration of CP1251 0xC0..0xDF (lowercase: the same in lower case).
TRANSLIT = "ABVGDEZZIJKLMNOPRSTUFHCCSSXYXEUA"
CP1251_UPPER_A = 0xC0
CP1251_LOWER_A = 0xE0
CP1251_UPPER_YO = 0xA8
CP1251_LOWER_YO = 0xB8
HIGH_BYTES = range(0x80, 0x100)
BAR_ROWS = 2

# FONT2D: a cell is always 18 rows tall with the baseline on row 14 (FONT2D_DrawLetter).
# font8x8 letters sit on 7 rows, so doubling the rows puts the baseline there.
FONT2D_ATLAS = 256
FONT2D_ROW_SCALE = 2
FONT2D_CELL_H = FONT8X8_PX * FONT2D_ROW_SCALE + BAR_ROWS
FONT2D_PITCH_X = FONT8X8_PX + 3  # one spare column between cells for the linear filter
FONT2D_PITCH_Y = FONT2D_CELL_H + 2
FONT2D_SPACING = 1  # gap after the ink, added to the advance

# Menu font: drawn at 1:1 at scale 256, additive. The pen advances by width - 1, so
# the gap between letters has to be blank atlas columns inside the glyph's width.
MENUFONT_ATLAS = 512
MENUFONT_COL_SCALE = 3
MENUFONT_ROW_SCALE = 4
MENUFONT_SPACING = 4
MENUFONT_CELL_W = FONT8X8_PX * MENUFONT_COL_SCALE + MENUFONT_SPACING
MENUFONT_CELL_H = FONT8X8_PX * MENUFONT_ROW_SCALE + BAR_ROWS
MENUFONT_PITCH_X = MENUFONT_CELL_W + 2
MENUFONT_PITCH_Y = MENUFONT_CELL_H + 2

# The bark that test mod overrides: a 16-bit mono sine (OpenAL is given 16-bit data).
VOICE_FILE = "voice/talk2/misc/ROPER-TAUNT03.wav"
VOICE_RATE = 22050
VOICE_SECONDS = 1
VOICE_TONE_HZ = 440
VOICE_AMPLITUDE = 12000
VOICE_SAMPLE_BYTES = 2


def user_data_root():
    if sys.platform == "win32":
        return Path(os.environ["APPDATA"]) / "OpenChaos"
    if sys.platform == "darwin":
        return Path.home() / "Library" / "Application Support" / "OpenChaos"
    base = os.environ.get("XDG_DATA_HOME") or str(Path.home() / ".local" / "share")
    return Path(base) / "OpenChaos"


def find_ci(folder, name):
    """Case-insensitive file lookup (the game data mixes cases)."""
    p = folder / name
    if p.exists():
        return p
    if folder.is_dir():
        for child in folder.iterdir():
            if child.name.lower() == name.lower():
                return child
    return None


def escape_key(raw):
    """English bytes -> table text: ASCII as is, everything else as \\xNN."""
    out = []
    for b in raw:
        c = chr(b)
        if c == "\\":
            out.append("\\\\")
        elif c == "\t":
            out.append("\\t")
        elif c == "\r":
            out.append("\\r")
        elif c == "\n":
            out.append("\\n")
        elif 0x20 <= b < 0x7F:
            out.append(c)
        else:
            out.append("\\x%02X" % b)
    return "".join(out)


def escape_value(text):
    return (text.replace("\\", "\\\\").replace("\t", "\\t")
            .replace("\r", "\\r").replace("\n", "\\n"))


def filler(tag, english_len, cap):
    want = max(math.ceil(english_len * GROWTH), len(tag) + 2)
    want = min(want, cap)
    body = tag + " "
    while len(body) < want:
        body += FILLER
    return body[:want].rstrip()


def make_value(tag, raw, cap):
    """Replaces every text piece between '|N' tokens / '|' separators / CRs / {glyphs}."""
    text = raw.decode("cp1252", errors="replace")
    parts = re.split(r"(\|\d*|\r|\{\w+\})", text)
    out = []
    for p in parts:
        if re.fullmatch(r"\|\d*|\r|\{\w+\}", p) or p == "":
            out.append(p)
        else:
            # keep the spaces around tokens so "Press |16 to ..." stays readable
            lead = p[:len(p) - len(p.lstrip(" "))]
            trail = p[len(p.rstrip(" ")):] if p.strip(" ") else ""
            out.append(lead + filler(tag, len(p), cap) + trail)
    return "".join(out)


def read_lang(game):
    path = find_ci(game / "text", "lang_english.txt")
    if not path:
        sys.exit("text/lang_english.txt not found in %s" % game)
    lines = []
    for line in path.read_bytes().split(b"\n"):
        line = line[:-1] if line.endswith(b"\r") else line
        if not line:
            break  # XLAT_load stops at the first empty line
        lines.append(line)
    return lines


def lang_ids():
    ids = {}
    for m in re.finditer(r"#define\s+(X_\w+)\s+\((\d+)\)", XLAT_HEADER.read_text(encoding="utf-8")):
        ids[m.group(1)] = int(m.group(2))
    return ids


def read_missions(game, levels):
    """(title, brief) bytes of the urban.sty lines whose .ucm is in levels."""
    path = find_ci(game / "data", "urban.sty")
    if not path:
        sys.exit("data/urban.sty not found in %s" % game)
    wanted = {l.lower() for l in levels}
    out = []
    for line in path.read_bytes().split(b"\n"):
        line = line.rstrip(b"\r")
        if not line or line.startswith(b"//") or line.startswith(b"["):
            continue
        fields = line.split(b":")
        if len(fields) < 10:  # Version 4: 9 fields before the briefing
            continue
        fn = fields[7].strip().decode("latin-1")
        if fn.lower() not in wanted:
            continue
        title = fields[8].strip(b" ")
        brief = b":".join(fields[9:])[1:]
        out.append((title, brief))
    return out


def read_ucm_messages(path):
    data = path.read_bytes()
    msgs = []
    i = 0
    while i < len(data) - 5:
        if data[i] == UCM_MESSAGE_TYPE:
            n = struct.unpack_from("<i", data, i + 1)[0]
            if 2 <= n <= UCM_MESSAGE_MAX and i + 5 + n <= len(data):
                text = data[i + 5:i + 5 + n].split(b"\0")[0]
                if (len(text) >= 4 and re.search(rb"[A-Za-z]{2}", text)
                        and all(c >= 0x20 for c in text)):
                    msgs.append(text)
                    i += 5 + n
                    continue
        i += 1
    citsez = data[UCM_CITSEZ_OFFSET:UCM_CITSEZ_OFFSET + UCM_NAME_SIZE].split(b"\0")[0]
    return msgs, citsez.decode("latin-1").replace("\\", "/")


def read_citizen_lines(path):
    """Numbered lines as EWAY_load_message_file keeps them."""
    out = []
    for line in path.read_bytes().split(b"\n"):
        if not re.match(rb"\s*-?\d+\.\s*\S", line):
            continue
        text = line[line.index(b".") + 1:].strip(b" \t\r\n\v\f")
        if text:
            out.append(text)
    return out


def check_mod_name(name):
    """The same rules as LOC_check_name in the game: one plain path component."""
    if not name:
        return "the name is empty"
    if len(name.encode("utf-8")) > MOD_NAME_MAX_BYTES:
        return "the name is longer than %d bytes" % MOD_NAME_MAX_BYTES
    if name == "." or ".." in name:
        return "the name contains '..'"
    if any(c in "/\\:" for c in name):
        return "the name contains a path separator or a drive colon"
    if any(ord(c) < 0x20 for c in name):
        return "the name contains a control character"
    return None


def read_font8x8():
    """The glyph rows of g_font8x8_basic, indexed by character code - FONT8X8_FIRST_CH."""
    text = FONT8X8_SOURCE.read_text(encoding="utf-8", errors="replace")
    start = text.index("g_font8x8_basic")
    row = r"\s*,\s*".join(["0x[0-9A-Fa-f]{2}"] * FONT8X8_PX)
    glyphs = [[int(v, 16) for v in re.findall("0x[0-9A-Fa-f]{2}", m.group(0))]
              for m in re.finditer(r"\{\s*" + row + r"\s*\}", text[start:])]
    if not glyphs:
        sys.exit("no font8x8 glyphs found in %s" % FONT8X8_SOURCE)
    return glyphs


def char_for_byte(b):
    """The ASCII character a high byte (or a lowercase Latin byte) is drawn as."""
    if CP1251_UPPER_A <= b < CP1251_UPPER_A + len(TRANSLIT):
        return TRANSLIT[b - CP1251_UPPER_A]
    if CP1251_LOWER_A <= b < CP1251_LOWER_A + len(TRANSLIT):
        return TRANSLIT[b - CP1251_LOWER_A].lower()
    if b == CP1251_UPPER_YO:
        return "E"
    if b == CP1251_LOWER_YO:
        return "e"
    if b < 0x80:
        return chr(b)
    return "%X" % (b & 0xF)


def ink_columns(rows):
    """First and last column with ink. A blank glyph counts as column 0."""
    cols = [c for c in range(FONT8X8_PX) if any(r & (1 << c) for r in rows)]
    return (cols[0], cols[-1]) if cols else (0, 0)


def blit_glyph(pixels, size, x0, y0, rows, col_scale, row_scale, value):
    """Draws a font8x8 glyph with a bar below it. Ink columns are moved to x0."""
    lo, hi = ink_columns(rows)
    for r, bits in enumerate(rows):
        for c in range(lo, hi + 1):
            if bits & (1 << c):
                for yy in range(r * row_scale, (r + 1) * row_scale):
                    for xx in range((c - lo) * col_scale, (c - lo + 1) * col_scale):
                        pixels[(y0 + yy) * size + x0 + xx] = value
    ink_w = (hi - lo + 1) * col_scale
    bar_y = y0 + FONT8X8_PX * row_scale
    for yy in range(BAR_ROWS):
        for xx in range(ink_w):
            pixels[(bar_y + yy) * size + x0 + xx] = value
    return ink_w


def write_tga(path, size, pixels):
    """Uncompressed 32-bit TGA, bottom-up. pixels: (b, g, r, a) tuples, top row first."""
    header = bytearray(18)
    header[2] = 2  # uncompressed true colour
    header[12:14] = struct.pack("<H", size)
    header[14:16] = struct.pack("<H", size)
    header[16] = 32
    header[17] = 8  # 8 alpha bits, bottom-left origin
    with open(path, "wb") as f:
        f.write(header)
        for y in range(size - 1, -1, -1):
            f.write(b"".join(bytes(p) for p in pixels[y * size:(y + 1) * size]))


def write_font2d(folder, glyphs):
    blank = (255, 255, 255, 0)  # white everywhere, so the linear filter never darkens an edge
    ink = (255, 255, 255, 255)
    pixels = [blank] * (FONT2D_ATLAS * FONT2D_ATLAS)
    cols = FONT2D_ATLAS // FONT2D_PITCH_X
    rows = ["# byte\tx\ty\tw\tadvance"]
    for i, b in enumerate(HIGH_BYTES):
        x = (i % cols) * FONT2D_PITCH_X
        y = (i // cols) * FONT2D_PITCH_Y
        g = glyphs[ord(char_for_byte(b)) - FONT8X8_FIRST_CH]
        w = blit_glyph(pixels, FONT2D_ATLAS, x, y, g, 1, FONT2D_ROW_SCALE, ink)
        rows.append("0x%02X\t%d\t%d\t%d\t%d" % (b, x, y, w, w + FONT2D_SPACING))
    write_tga(folder / "font2d.tga", FONT2D_ATLAS, pixels)
    (folder / "font2d.map").write_text("\n".join(rows) + "\n", encoding="utf-8", newline="\n")


def write_menufont(folder, glyphs):
    blank = (0, 0, 0, 0)  # the page is additive: intensity lives in RGB
    ink = (255, 255, 255, 255)
    pixels = [blank] * (MENUFONT_ATLAS * MENUFONT_ATLAS)
    cols = MENUFONT_ATLAS // MENUFONT_PITCH_X
    rows = ["# byte\tx\ty\tw\th"]
    for i, b in enumerate(list(HIGH_BYTES) + list(range(ord("a"), ord("z") + 1))):
        x = (i % cols) * MENUFONT_PITCH_X
        y = (i // cols) * MENUFONT_PITCH_Y
        g = glyphs[ord(char_for_byte(b)) - FONT8X8_FIRST_CH]
        w = blit_glyph(pixels, MENUFONT_ATLAS, x, y, g, MENUFONT_COL_SCALE, MENUFONT_ROW_SCALE, ink)
        rows.append("0x%02X\t%d\t%d\t%d\t%d" % (b, x, y, w + MENUFONT_SPACING, MENUFONT_CELL_H))
    write_tga(folder / "menufont.tga", MENUFONT_ATLAS, pixels)
    (folder / "menufont.map").write_text("\n".join(rows) + "\n", encoding="utf-8", newline="\n")


def write_voice(folder):
    path = folder / VOICE_FILE
    path.parent.mkdir(parents=True, exist_ok=True)
    frames = bytearray()
    for i in range(VOICE_RATE * VOICE_SECONDS):
        value = VOICE_AMPLITUDE * math.sin(2 * math.pi * VOICE_TONE_HZ * i / VOICE_RATE)
        frames += struct.pack("<h", int(value))
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(VOICE_SAMPLE_BYTES)
        w.setframerate(VOICE_RATE)
        w.writeframes(bytes(frames))


def write_mod(folder, display_name, rows, table_name="strings.tsv", extras=None):
    folder.mkdir(parents=True, exist_ok=True)
    manifest = {
        "name": "Test mod",
        "version": "1",
        "display_name": display_name,
        "table": table_name,
        "codepage": "CP1251",
    }
    manifest.update(extras or {})
    (folder / "mod.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    with open(folder / table_name, "w", encoding="utf-8", newline="\n") as f:
        f.write("# OpenChaos localization test mod (generated by make_testmod.py)\n")
        f.write("# key <TAB> value <TAB> context\n")
        for key, value, ctx in rows:
            f.write("%s\t%s\t%s\n" % (key, value, ctx))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--game-dir", type=Path, default=REPO / "original_game_resources")
    ap.add_argument("--out-root", type=Path, default=None)
    ap.add_argument("--name", default="testmod")
    ap.add_argument("--levels", default=DEFAULT_LEVELS)
    ap.add_argument("--broken", action="store_true")
    ap.add_argument("--select", default=None)
    args = ap.parse_args()

    bad_name = check_mod_name(args.name)
    if bad_name:
        ap.error("--name %r: %s" % (args.name, bad_name))

    game = args.game_dir
    out_root = args.out_root or user_data_root()
    levels = [l.strip() for l in args.levels.split(",") if l.strip()]

    rows = []
    seen = set()

    def add(raw, ctx, cap):
        key = escape_key(raw)
        if (key, ctx) in seen:
            return
        seen.add((key, ctx))
        tag = "T%d" % (len(rows) + 1)
        rows.append((key, escape_value(make_value(tag, raw, cap)), ctx))

    # lang
    lang = read_lang(game)
    ids = lang_ids()
    for name in LANG_IDS:
        i = ids.get(name)
        if i is not None and i < len(lang):
            add(lang[i], "lang", MESSAGE_LINE_CAP)
    for text in OWN_STRINGS:
        add(text.encode("ascii"), "own", MESSAGE_LINE_CAP)
    # mission titles and briefings: the briefing gets a second line
    for title, brief in read_missions(game, levels):
        add(title, "sty", TITLE_CAP)
        key = escape_key(brief)
        if (key, "sty") not in seen:
            seen.add((key, "sty"))
            tag = "T%d" % (len(rows) + 1)
            half = filler(tag, math.ceil(len(brief) * GROWTH / 2), BRIEF_HALF_CAP)
            rows.append((key, escape_value(half + "\r" + half), "sty"))
    # level messages, and the citizen files they name
    citizen_files = {"text/citsez.txt"}
    for level in levels:
        path = find_ci(game / "levels", level)
        if not path:
            print("warning: level %s not found, skipped" % level)
            continue
        msgs, citsez = read_ucm_messages(path)
        for m in msgs:
            add(m, "ucm", MESSAGE_LINE_CAP)
        if citsez:
            citizen_files.add(citsez)
    for rel in sorted(citizen_files):
        folder, _, fname = rel.rpartition("/")
        path = find_ci(game / folder, fname) if folder else find_ci(game, fname)
        if not path:
            print("warning: citizen file %s not found, skipped" % rel)
            continue
        for line in read_citizen_lines(path):
            add(line, "txt", MESSAGE_LINE_CAP)

    mods = out_root / "localizations"
    write_mod(mods / args.name, "Тестовый", rows, extras={
        "font2d": {"image": "font2d.tga", "map": "font2d.map"},
        "menufont": {"image": "menufont.tga", "map": "menufont.map"},
        "voice": True,
    })
    font8x8 = read_font8x8()
    write_font2d(mods / args.name, font8x8)
    write_menufont(mods / args.name, font8x8)
    write_voice(mods / args.name)
    print("wrote %s (%d rows, fonts, voice)" % (mods / args.name, len(rows)))

    if args.broken:
        (mods / (args.name + "_nomanifest")).mkdir(parents=True, exist_ok=True)
        bad = mods / (args.name + "_badjson")
        bad.mkdir(parents=True, exist_ok=True)
        (bad / "mod.json").write_text("{ \"display_name\": ", encoding="utf-8")
        notable = mods / (args.name + "_notable")
        write_mod(notable, "No table", rows[:1])
        manifest = json.loads((notable / "mod.json").read_text(encoding="utf-8"))
        del manifest["table"]
        (notable / "mod.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        badtable = mods / (args.name + "_badtable")
        write_mod(badtable, "Bad table", rows[:1] + [("bad \\q escape", "value", "")])
        print("wrote broken mods: %s_nomanifest, _badjson, _notable, _badtable" % args.name)

    if args.select is not None:
        cfg_path = out_root / "OpenChaos.config.json"
        cfg = json.loads(cfg_path.read_text(encoding="utf-8")) if cfg_path.exists() else {}
        cfg.setdefault("localization", {})["mod"] = args.select
        cfg_path.parent.mkdir(parents=True, exist_ok=True)
        cfg_path.write_text(json.dumps(cfg, ensure_ascii=False, indent=4) + "\n", encoding="utf-8")
        print("set localization.mod = \"%s\" in %s" % (args.select, cfg_path))


if __name__ == "__main__":
    main()
