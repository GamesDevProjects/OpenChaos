#!/usr/bin/env python3
"""Generate a synthetic localization test mod for OpenChaos.

Two kinds of test mod, chosen with --mode. Neither holds a real translation.

--mode index (default): a coverage mod. Every value is its English key with
the row's index in front, in angle quotes: "Start New Game" becomes
"«0»Start New Game". The quotes are CP1251 0xAB and 0xBB, and the mod ships a
minimal font that draws only these two bytes, as square brackets with a 2 px
bar under them, in both the FONT2D and the menu atlas. In game the text reads
"[0]Start New Game":
  - a barred [N] in front: the table reached the text and the mod font drew it;
  - '?', a blank or a stray glyph around N: the table reached the text but the
    mod font did not load (OpenChaos's own strings stay plain English then,
    because they are checked against the font);
  - no index at all: a string the table does not reach.
N is the 0-based data row of strings.tsv; it sits on file line N + 3, the line
number the game's "table row" log messages use. Each line of a value, and each
part of a '|'-separated conversation, gets the index; |N and {glyph} tokens are
kept. The mod covers every key the script can find: all lang lines but the
language name, every mission title and briefing in urban.sty, every message of
every level in levels/, every numbered line of the text/*.txt message files
(not the other languages' citsez_*.txt), and OpenChaos's own strings, the quick
save messages included. A value that would break a loader limit with the index
is written without it, so the row still applies; the script prints how many
and names each. With the retail data that is one row: a developer test message
in levels/wibble.ucm, over the message line limit even in English, so the game
keeps its English text. The loader refuses a '%' in UI strings and messages, so
there the value has '@' in place of each '%' (the key keeps its '%'). Apart
from the two quotes and that stand-in every byte stays as in the English data.
No voice.

--mode font: the full-alphabet mod. It exercises every text hook with a sample
of rows: each value is a tagged Cyrillic filler ("T12 проверка ...") at least
40 % longer than its English key, so the longer-string and CP1251 paths are both
used. The tag ("T<row>") is plain ASCII, so it stays readable even before a mod
font exists.

No game text is stored in this script or in the repository: the English keys
are read from YOUR copy of the game data at generation time, and the mod is
written into the user data folder (outside the repository).

Rows written by --mode font (keys taken from the game data):
  lang  main-menu and options labels, the pause menu's save/load items and its
        confirm dialog, plus "Press |16 to continue." style rows (keeps the |N token)
  sty   title and briefing of the chosen missions (briefing becomes 2 lines)
  ucm   every dialogue/message record of the chosen levels ('|' separators kept)
  txt   every citizen line of text/citsez.txt and of the levels' own citizen files
  own   strings OpenChaos itself added: the pause menu's Help item, the help topic
        titles, one help line with its {glyph} token, the Options menu's Language
        item and restart note, and every message of the in-mission quick save
        (keys are in OWN_STRINGS and SAVES_STRINGS below, not game data)

Other files written by --mode font:
  font2d.tga + font2d.map      FONT2D atlas for bytes 0x80..0xFF
  menufont.tga + menufont.map  menu atlas for bytes 0x80..0xFF and lowercase a..z
  voice/talk2/misc/ROPER-TAUNT03.wav  a 1 s sine, so the overridden bark is audible
  voice/talk2/<level>.ucm<N>.wav  a 5 s sine for each tutorial message of the
        chosen levels (FTutor1), which the original game never voices: it
        sounds when the message pops up and plays on after it is closed
  voice/talk2/civsdead_warning<N>.wav  a 10 s sine for each of the four
        civilian casualty warnings after a won mission, which the original game
        never voices: it sounds when the warning opens and stops when it is closed
  voice/bink/PCINTRO_withsound.wav  a sound track for the intro movie: a short
        beep each second, then silence, so the movie plays the game's picture
        with this track in place of its own sound
--mode index writes the same four font files with the bytes 0xAB and 0xBB only.

The glyphs are the public-domain font8x8 already in the repository, read from
new_game/src/game/missing_resources.cpp. It has no Cyrillic, so each CP1251
letter draws its Latin transliteration and the other high bytes draw the last
hex digit of the byte. Every glyph carries a 2 px bar along its bottom: a letter
with a bar comes from the mod atlas, a letter without one from the game's font.

Usage:
  python make_testmod.py [--mode index|font] [--game-dir DIR] [--out-root DIR]
                         [--name testmod]
                         [--levels Assault1.ucm,FTutor1.ucm,police1.ucm]
                         [--broken] [--select NAME|""]

  --mode      index (default) or font, see above.
  --levels    the levels --mode font covers. --mode index always covers all.

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
# The help screens' topic titles and per-device bodies (OpenChaos's own text).
HELP_DIR = REPO / "new_game" / "src" / "ui" / "menus"
HELP_TOPICS_SOURCE = HELP_DIR / "help_content.cpp"
HELP_BODY_SOURCES = ["help_content_common.h", "help_content_kbm.cpp", "help_content_xbox.cpp", "help_content_ps.cpp"]

# Lang ids translated by the test mod (main menu, options, |N prompts, and the
# pause menu's quick save items with their confirm dialog).
LANG_IDS = [
    "X_START", "X_LOAD_GAME", "X_OPTIONS", "X_CREDITS", "X_EXIT",
    "X_SOUND", "X_VOLUME", "X_AMBIENT", "X_MUSIC",
    "X_PRESS_TO_CONTINUE", "X_ENTER_VEHICLE",
    "X_SAVE_GAME", "X_ARE_YOU_SURE", "X_OKAY", "X_CANCEL",
]

DEFAULT_LEVELS = "Assault1.ucm,FTutor1.ucm,police1.ucm"

# Strings OpenChaos itself added. These are not game data, so they are spelled
# out here. The help line has a {glyph} token, which its value has to keep.
OWN_STRINGS = [
    "Help", "CONTROLS", "MOVEMENT", "COMBAT", "WEAPONS", "DRIVING", "OTHER",
    "{kb_space} - Jump.", "Language", "Restart to apply", "Bonus mission unlocked!",
]

# The in-mission quick save's messages (new_game/src/missions/quicksave.cpp): the
# panel messages, the pause menu's refusal lines and each line of the two
# campaign-progress warnings. Only shown when the game is also built with
# ext_in_mission_saves = true.
SAVES_STRINGS = [
    "GAME SAVED", "GAME LOADED", "QUICK SAVE CANNOT BE LOADED",
    "CANNOT SAVE WHILE IN A VEHICLE", "CANNOT SAVE DURING A FIGHT",
    "CANNOT SAVE WHILE AN ENEMY IS ENGAGED", "CANNOT SAVE DURING A CUTSCENE",
    "CANNOT SAVE RIGHT NOW",
    "THE CAMPAIGN PROGRESS IN THIS QUICK SAVE IS OLDER",
    "THAN YOUR CURRENT PROGRESS. LOADING IT WILL",
    "DISCARD THE PROGRESS MADE SINCE.",
    "THE CAMPAIGN PROGRESS IN THIS QUICK SAVE IS",
    "DIFFERENT FROM YOUR CURRENT PROGRESS. LOADING IT",
    "REPLACES YOUR CURRENT PROGRESS WITH THE SAVED ONE.",
]

# Every value is at least this much longer than its key.
GROWTH = 1.4
# Byte caps that keep every generated value inside the engine's limits:
# one displayed message line, a mission title, and half a briefing.
MESSAGE_LINE_CAP = 240
TITLE_CAP = 200
BRIEF_HALF_CAP = 800

# --mode index: the index quotes, CP1251 0xAB and 0xBB, written to the UTF-8
# table as U+00AB and U+00BB, and the font8x8 glyphs the mod font draws them with.
INDEX_OPEN_BYTE = 0xAB
INDEX_CLOSE_BYTE = 0xBB
INDEX_CHARS = {INDEX_OPEN_BYTE: "\u00ab", INDEX_CLOSE_BYTE: "\u00bb"}
INDEX_GLYPHS = [(INDEX_OPEN_BYTE, "["), (INDEX_CLOSE_BYTE, "]")]
# The loader refuses a '%' in the values of these contexts: UI strings and
# messages reach printf-style functions as the format string. A key may keep
# its '%', so --mode index writes this stand-in in the value instead. '@' is
# drawn by every game font and means nothing to the message and UI string
# paths; its one special use, a line break in .sty briefings, is in a context
# outside this list.
PERCENT_REFUSED_CONTEXTS = ("lang", "ucm", "txt")
PERCENT = b"%"
PERCENT_STAND_IN = b"@"
# How much of a key the report of rows without the index shows.
PLAIN_KEY_SHOWN = 60
# strings.tsv starts with this many comment lines, and the loader numbers file
# lines from 1, so data row N is on line N + INDEX_FIRST_LINE.
TABLE_HEADER_LINES = 2
INDEX_FIRST_LINE = TABLE_HEADER_LINES + 1

# Loader limits (new_game/src/localization/localization.cpp), in bytes after the
# conversion to the codepage. A value over one is written without its index.
# A UI string after XLAT_str expands it: sizeof(xlat_buffer) - 1 = 260 + 100 - 1.
LANG_EXPANDED_MAX = 260 + 100 - 1
# XLAT_str puts a key name of up to 8 bytes in place of a |N token, and the pool
# stores a token as 2 bytes.
LANG_TOKEN_EXPANDED = 8
LANG_TOKEN_PACKED = 2
# The pool (MAX_STRING_SPACE) less what XLAT_init appends: one name of 8 bytes
# plus its terminator for each control id 1..XCTL_CAM_CONTINUE (16).
LANG_POOL_BYTES = 8192 - 16 * (LANG_TOKEN_EXPANDED + 1)
# Lang strings copied together into one buffer (LOC_LANG_GROUPS): the ids, the
# terminators and format text around them, the buffer size.
LANG_GROUPS = [
    (("X_NO", "X_YES"), 2, 20),
    (("X_CAMERA", "X_CHARACTER"), 2, 255),
    (("X_NO_CONES_HIT",), 1, 64),
    (("X_NUM_CONES_HIT",), 3 + 11 + 1, 64),
    (("X_CON_INCREASED", "X_STA_INCREASED", "X_STR_INCREASED", "X_REF_INCREASED"),
     4 * (2 + 3) + 3 * 2 + 1 + 1, 200),
    (("X_GET_DOWN",), 1, 300),
    (("X_CANT_SHOOT_COP",), 1, 300),
    (("X_EMPTY",), 1, 2048 // 10),
]
# One displayed message line (between '|' separators), and a whole record.
MESSAGE_LINE_MAX = 300 - 1
MESSAGE_RECORD_MAX = 1024 - 1
# All messages of a level (EWAY_MESS_BUFFER_SIZE), terminators included.
MESSAGE_POOL_BYTES = 16384
# A mission title (and save slot label), and a briefing.
TITLE_MAX = 2048 // 10 - 1
BRIEF_MAX = 2048 - 255 - 1
# OpenChaos's own strings have no limit, except the quick save's, which include
# panel messages (LOC_own_message): one message line.
SAVES_MAX = MESSAGE_LINE_MAX
# Message files every level loads besides its own (EWAY_load_fake_wander_text).
SHARED_MESSAGE_FILES = ["citsez.txt", "guilty.txt", "annoyed.txt"]
# Message files of other languages: they never apply with English lang data.
FOREIGN_TEXT_PREFIXES = ("lang_", "citsez_")

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
# Tutorial message tones: long enough to outlast the message, so it can be heard
# that the line plays on after the player closes it, and lower than the bark.
TUTORIAL_VOICE_SECONDS = 5
TUTORIAL_VOICE_TONE_HZ = 330
# Civilian casualty warning tones: longer than anyone reads the warning, so it
# can be heard that the line stops when the player closes it.
CIV_WARNING_FILE = "voice/talk2/civsdead_warning%d.wav"
CIV_WARNING_COUNT = 4  # X_CIVSDEAD_WARNING_1 .. X_CIVSDEAD_WARNING_4
CIV_WARNING_SECONDS = 10
CIV_WARNING_TONE_HZ = 262
# Intro movie sound track: a beep at the start of each second, so it is plain
# that the movie's own sound is gone; the track is shorter than the movie, and
# the rest of the movie is silent.
MOVIE_TRACK_FILE = "voice/bink/PCINTRO_withsound.wav"
MOVIE_TRACK_SECONDS = 20
MOVIE_TRACK_BEEP_SECONDS = 0.2
MOVIE_TRACK_TONE_HZ = 880

# .ucm event points follow the header: version, used, five names, then four
# UWORDs (MapIndex, Used, Free, crime rate + fake civs); see ELEV_load_level in
# new_game/src/assets/formats/elev.cpp. Each point is a 14-byte header and 60
# bytes of Data[10], Radius, X, Y, Z, Next, Prev (EventPoint, missions/mission.h).
UCM_EVENT_POINTS_OFFSET = 4 + 4 + 5 * UCM_NAME_SIZE + 4 * 2
UCM_EVENT_POINT_SIZE = 14 + 60
UCM_MAX_EVENT_POINTS = 512  # MAX_EVENTPOINTS
UCM_EP_WAYPOINT_TYPE = 2  # byte offset of WaypointType
UCM_EP_USED = 3  # byte offset of Used
UCM_EP_DATA = 14  # byte offset of Data[0]
UCM_WPT_MESSAGE = 12  # WPT_MESSAGE
UCM_MESSAGE_WHO = 2  # Data[2]: who says a message
UCM_MESSAGE_SAMPLE = 9  # Data[9]: the voice file number
UCM_WHO_TUTORIAL = 0xFFFE  # EWAY_MESSAGE_WHO_TUTORIAL


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


def has_control_byte(raw):
    """True if raw has a control byte the table cannot hold: only TAB, CR and LF have
    escapes, and a \\xNN below 0x20 makes the loader reject the whole table."""
    return any(b < 0x20 and b not in b"\t\r\n" for b in raw)


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
    """(title, brief) bytes of the urban.sty lines whose .ucm is in levels (all if None)."""
    path = find_ci(game / "data", "urban.sty")
    if not path:
        sys.exit("data/urban.sty not found in %s" % game)
    wanted = {l.lower() for l in levels} if levels is not None else None
    out = []
    for line in path.read_bytes().split(b"\n"):
        line = line.rstrip(b"\r")
        if not line or line.startswith(b"//") or line.startswith(b"["):
            continue
        fields = line.split(b":")
        if len(fields) < 10:  # Version 4: 9 fields before the briefing
            continue
        fn = fields[7].strip().decode("latin-1")
        if wanted is not None and fn.lower() not in wanted:
            continue
        title = fields[8].strip(b" ")
        # The game turns every '@' into a line break (CR) and looks the briefing up
        # without its trailing line end (FRONTEND_ParseMissionData, LOC_mission_data).
        brief = b":".join(fields[9:])[1:].replace(b"@", b"\r").rstrip(b"\r\n")
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


def write_font2d(folder, glyphs, chars):
    """chars: (byte, ASCII character drawn for it) pairs."""
    blank = (255, 255, 255, 0)  # white everywhere, so the linear filter never darkens an edge
    ink = (255, 255, 255, 255)
    pixels = [blank] * (FONT2D_ATLAS * FONT2D_ATLAS)
    cols = FONT2D_ATLAS // FONT2D_PITCH_X
    rows = ["# byte\tx\ty\tw\tadvance"]
    for i, (b, ch) in enumerate(chars):
        x = (i % cols) * FONT2D_PITCH_X
        y = (i // cols) * FONT2D_PITCH_Y
        g = glyphs[ord(ch) - FONT8X8_FIRST_CH]
        w = blit_glyph(pixels, FONT2D_ATLAS, x, y, g, 1, FONT2D_ROW_SCALE, ink)
        rows.append("0x%02X\t%d\t%d\t%d\t%d" % (b, x, y, w, w + FONT2D_SPACING))
    write_tga(folder / "font2d.tga", FONT2D_ATLAS, pixels)
    (folder / "font2d.map").write_text("\n".join(rows) + "\n", encoding="utf-8", newline="\n")


def write_menufont(folder, glyphs, chars):
    """chars: (byte, ASCII character drawn for it) pairs."""
    blank = (0, 0, 0, 0)  # the page is additive: intensity lives in RGB
    ink = (255, 255, 255, 255)
    pixels = [blank] * (MENUFONT_ATLAS * MENUFONT_ATLAS)
    cols = MENUFONT_ATLAS // MENUFONT_PITCH_X
    rows = ["# byte\tx\ty\tw\th"]
    for i, (b, ch) in enumerate(chars):
        x = (i % cols) * MENUFONT_PITCH_X
        y = (i // cols) * MENUFONT_PITCH_Y
        g = glyphs[ord(ch) - FONT8X8_FIRST_CH]
        w = blit_glyph(pixels, MENUFONT_ATLAS, x, y, g, MENUFONT_COL_SCALE, MENUFONT_ROW_SCALE, ink)
        rows.append("0x%02X\t%d\t%d\t%d\t%d" % (b, x, y, w + MENUFONT_SPACING, MENUFONT_CELL_H))
    write_tga(folder / "menufont.tga", MENUFONT_ATLAS, pixels)
    (folder / "menufont.map").write_text("\n".join(rows) + "\n", encoding="utf-8", newline="\n")


def read_tutorial_samples(path):
    """Voice file numbers of the level's tutorial messages, in event point order."""
    data = path.read_bytes()
    samples = []
    for i in range(UCM_MAX_EVENT_POINTS):
        at = UCM_EVENT_POINTS_OFFSET + i * UCM_EVENT_POINT_SIZE
        if at + UCM_EVENT_POINT_SIZE > len(data):
            break
        if not data[at + UCM_EP_USED] or data[at + UCM_EP_WAYPOINT_TYPE] != UCM_WPT_MESSAGE:
            continue
        values = struct.unpack_from("<10i", data, at + UCM_EP_DATA)
        if values[UCM_MESSAGE_WHO] == UCM_WHO_TUTORIAL:
            samples.append(values[UCM_MESSAGE_SAMPLE])
    return samples


def write_tutorial_voices(folder, game, levels):
    """A tone for each tutorial message of the levels. Returns how many were written."""
    count = 0
    for level in levels:
        path = find_ci(game / "levels", level)
        if not path:
            continue
        stem = path.stem
        for sample in read_tutorial_samples(path):
            write_tone(folder / "voice" / "talk2" / ("%s.ucm%d.wav" % (stem, sample)),
                       TUTORIAL_VOICE_SECONDS, TUTORIAL_VOICE_TONE_HZ)
            count += 1
    return count


def write_voice(folder):
    write_tone(folder / VOICE_FILE, VOICE_SECONDS, VOICE_TONE_HZ)
    for number in range(1, CIV_WARNING_COUNT + 1):
        write_tone(folder / (CIV_WARNING_FILE % number), CIV_WARNING_SECONDS, CIV_WARNING_TONE_HZ)
    write_tone(folder / MOVIE_TRACK_FILE, MOVIE_TRACK_SECONDS, MOVIE_TRACK_TONE_HZ,
               MOVIE_TRACK_BEEP_SECONDS)


def write_tone(path, seconds, tone_hz, beep_seconds=None):
    """A sine; with beep_seconds, only the first beep_seconds of each second sound."""
    path.parent.mkdir(parents=True, exist_ok=True)
    frames = bytearray()
    for i in range(VOICE_RATE * seconds):
        value = VOICE_AMPLITUDE * math.sin(2 * math.pi * tone_hz * i / VOICE_RATE)
        if beep_seconds is not None and (i % VOICE_RATE) >= VOICE_RATE * beep_seconds:
            value = 0
        frames += struct.pack("<h", int(value))
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(VOICE_SAMPLE_BYTES)
        w.setframerate(VOICE_RATE)
        w.writeframes(bytes(frames))


def write_mod(folder, display_name, rows, table_name="strings.tsv", extras=None,
              header="# OpenChaos localization test mod (generated by make_testmod.py)"):
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
        f.write(header + "\n")
        f.write("# key <TAB> value <TAB> context\n")
        for key, value, ctx in rows:
            f.write("%s\t%s\t%s\n" % (key, value, ctx))


# C source tokens: a string literal, a comment, an identifier, or any other character.
C_TOKEN = re.compile(r'"((?:[^"\\]|\\.)*)"|//[^\n]*|/\*.*?\*/|([A-Za-z_]\w*)|(.)', re.S)
C_ESCAPES = {"n": "\n", "t": "\t", '"': '"', "\\": "\\", "'": "'"}


def c_unescape(text):
    return re.sub(r"\\(.)", lambda m: C_ESCAPES.get(m.group(1), m.group(1)), text)


def read_help_strings():
    """The help topic titles, then every line of every help body, as the game looks
    them up. Bodies are C string literals and #define'd fragments concatenated."""
    # Topics without bodies (nullptr) are developer tools, not drawn in normal play.
    titles = re.findall(r'\{\s*"((?:[^"\\]|\\.)*)"\s*,(?!\s*nullptr)', HELP_TOPICS_SOURCE.read_text(encoding="utf-8"))
    macros = {}
    lines = []
    for name in HELP_BODY_SOURCES:
        text = (HELP_DIR / name).read_text(encoding="utf-8")
        for m in re.finditer(r'#define\s+(\w+)\s*\\?\s*((?:"(?:[^"\\]|\\.)*"\s*)+)', text):
            macros[m.group(1)] = "".join(c_unescape(x) for x in re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(2)))
        body = None
        for tok in C_TOKEN.finditer(text):
            literal, ident, other = tok.group(1), tok.group(2), tok.group(3)
            if body is None:
                if other == "=":
                    body = []
                continue
            if literal is not None:
                body.append(c_unescape(literal))
            elif ident in macros:
                body.append(macros[ident])
            elif other == ";":
                lines += [l for l in "".join(body).split("\n") if l]
                body = None
    return [c_unescape(t) for t in titles] + lines


def index_prefix(n):
    return bytes([INDEX_OPEN_BYTE]) + str(n).encode("ascii") + bytes([INDEX_CLOSE_BYTE])


def insert_index(raw, prefix, conversation):
    """raw with prefix at the start of every line, and for a conversation also after
    every '|' separator. A '|' with digits is one token to the loader, so the index
    goes after the digits and the token stays whole."""
    out = bytearray()
    at_start = True
    i = 0
    while i < len(raw):
        c = raw[i]
        if c in b"\r\n":
            out.append(c)
            at_start = True
            i += 1
            continue
        if at_start and not (conversation and c == ord("|")):
            out += prefix
            at_start = False
        if conversation and c == ord("|"):
            j = i + 1
            while j < len(raw) and raw[j] in b"0123456789":
                j += 1
            out += raw[i:j]
            at_start = True
            i = j
            continue
        out.append(c)
        i += 1
    return bytes(out)


def pipe_tokens(raw):
    return re.findall(rb"\|\d*", raw)


def lang_key_tokens(raw):
    """The |N tokens XLAT_load compresses; a bare '|' stays a literal byte."""
    return [t for t in pipe_tokens(raw) if len(t) > 1]


def lang_packed_len(raw):
    return len(raw) - sum(len(t) - LANG_TOKEN_PACKED for t in lang_key_tokens(raw))


def lang_expanded_len(raw):
    return len(raw) - sum(len(t) - LANG_TOKEN_EXPANDED for t in lang_key_tokens(raw))


def longest_part(raw):
    return max(len(p) for p in raw.split(b"|"))


def fits(kind, value):
    """The per-row loader limits of a value (bytes in the codepage)."""
    if kind in ("lang", "ucm", "txt") and b"%" in value:
        return False  # refused by the loader whatever the value: the row stays English
    if kind == "lang":
        return lang_expanded_len(value) <= LANG_EXPANDED_MAX
    if kind in ("ucm", "txt"):
        return longest_part(value) <= MESSAGE_LINE_MAX and len(value) <= MESSAGE_RECORD_MAX
    if kind == "title":
        return len(value) <= TITLE_MAX
    if kind == "brief":
        return len(value) <= BRIEF_MAX
    if kind == "saves":
        return len(value) <= SAVES_MAX
    return True


def escape_index_value(value):
    """Value bytes -> table text: the index quotes as UTF-8, the rest as escape_key."""
    return "".join(INDEX_CHARS.get(b) or escape_key(bytes([b])) for b in value)


def build_index_rows(game):
    """Every key the script finds, valued with its row index. Returns the table rows
    and a (row, source, English text fits, key) tuple for each row written without
    the index."""
    entries = []  # dicts: key, ctx (table context), kind (limit class), src (file), value
    by_key = {}

    def add(raw, ctx, src, kind=None):
        if has_control_byte(raw):
            print("warning: %s key with a control byte skipped: %r" % (ctx, raw[:60]))
            return None
        # ucm and txt rows share one lookup in the loader, so one row per key there.
        surface = "msg" if ctx in ("ucm", "txt") else ctx
        if (raw, surface) in by_key:
            return by_key[(raw, surface)]
        e = {"key": raw, "ctx": ctx, "kind": kind or ctx, "src": src}
        text = raw.replace(PERCENT, PERCENT_STAND_IN) if ctx in PERCENT_REFUSED_CONTEXTS else raw
        e["value"] = insert_index(text, index_prefix(len(entries)), ctx in ("ucm", "txt"))
        entries.append(e)
        by_key[(raw, surface)] = e
        return e

    lang = read_lang(game)
    lang_rows = {}
    for i, line in enumerate(lang):
        if i != 0:  # line 0 names the language and is never translated
            e = add(line, "lang", "text/lang_english.txt")
            if e:
                lang_rows[i] = e
    for title, brief in read_missions(game, None):
        add(title, "sty", "data/urban.sty", "title")
        add(brief, "sty", "data/urban.sty", "brief")

    text_dir = game / "text"
    message_files = {}
    for path in sorted(text_dir.iterdir()) if text_dir.is_dir() else []:
        name = path.name.lower()
        if path.suffix.lower() == ".txt" and not name.startswith(FOREIGN_TEXT_PREFIXES):
            message_files[name] = [e for e in (add(line, "txt", "text/" + path.name) for line in read_citizen_lines(path))
                                   if e]
    shared = [e for f in SHARED_MESSAGE_FILES for e in message_files.get(f, [])]

    levels_dir = game / "levels"
    level_messages = []
    for path in sorted(levels_dir.iterdir()) if levels_dir.is_dir() else []:
        if path.suffix.lower() != ".ucm":
            continue
        msgs, citsez = read_ucm_messages(path)
        own = [e for e in (add(m, "ucm", "levels/" + path.name) for m in msgs) if e]
        named = message_files.get(citsez.rpartition("/")[2].lower(), [])
        level_messages.append((own, own + named + shared))

    for text in OWN_STRINGS + read_help_strings():
        add(text.encode("utf-8"), "own", "OpenChaos")  # the bytes of the compiled literal
    for text in SAVES_STRINGS:
        add(text.encode("ascii"), "own", "OpenChaos", "saves")

    def plain(e):
        e["value"] = e["key"]

    for e in entries:
        if not fits(e["kind"], e["value"]):
            plain(e)

    ids = lang_ids()
    for names, overhead, capacity in LANG_GROUPS:
        members = [lang_rows[ids[n]] for n in names if ids.get(n) in lang_rows]
        if overhead + sum(lang_expanded_len(e["value"]) for e in members) > capacity:
            for e in members:
                plain(e)

    # The whole lang pool, line 0 included: drop indexes from the end until it fits.
    def pool():
        return len(lang[0]) + 1 + sum(lang_packed_len(e["value"]) + 1 for e in lang_rows.values())
    for e in reversed(list(lang_rows.values())):
        if pool() <= LANG_POOL_BYTES:
            break
        plain(e)

    # Each level's message pool: drop the level's own indexes, longest first.
    for own, loaded in level_messages:
        total = sum(len(e["value"]) + 1 for e in loaded)
        for e in sorted(own, key=lambda e: -len(e["value"])):
            if total <= MESSAGE_POOL_BYTES:
                break
            total -= len(e["value"]) - len(e["key"])
            plain(e)

    rows = [(escape_key(e["key"]), escape_index_value(e["value"]), e["ctx"]) for e in entries]
    plain_rows = [(n, e["src"], fits(e["kind"], e["key"]), e["key"])
                  for n, e in enumerate(entries) if e["value"] == e["key"]]
    return rows, plain_rows


def build_font_rows(game, levels):
    rows = []
    seen = set()

    def add(raw, ctx, cap):
        if has_control_byte(raw):
            print("warning: %s key with a control byte skipped: %r" % (ctx, raw[:60]))
            return
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
    for text in OWN_STRINGS + SAVES_STRINGS:
        add(text.encode("ascii"), "own", MESSAGE_LINE_CAP)
    # mission titles and briefings: the briefing gets a second line
    for title, brief in read_missions(game, levels):
        add(title, "sty", TITLE_CAP)
        key = escape_key(brief)
        if (key, "sty") not in seen and not has_control_byte(brief):
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
    return rows


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--mode", choices=("index", "font"), default="index")
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

    mods = out_root / "localizations"
    fonts = {
        "font2d": {"image": "font2d.tga", "map": "font2d.map"},
        "menufont": {"image": "menufont.tga", "map": "menufont.map"},
    }
    font8x8 = read_font8x8()
    if args.mode == "index":
        rows, plain = build_index_rows(game)
        write_mod(mods / args.name, "Index test", rows, extras=fonts,
                  header="# OpenChaos localization index test mod (generated by make_testmod.py);"
                         " data row N is on line N + %d" % INDEX_FIRST_LINE)
        write_font2d(mods / args.name, font8x8, INDEX_GLYPHS)
        write_menufont(mods / args.name, font8x8, INDEX_GLYPHS)
        print("wrote %s (%d rows, %d of them without the index because of a loader limit, index font)"
              % (mods / args.name, len(rows), len(plain)))
        for n, src, english_fits, key in plain:
            why = ("the index would break a loader limit, the row applies with its English text" if english_fits
                   else "over a loader limit even in English, the game keeps its English text")
            print("  row %d (%s): %s: %s" % (n, src, why, escape_key(key[:PLAIN_KEY_SHOWN])))
    else:
        rows = build_font_rows(game, levels)
        write_mod(mods / args.name, "Тестовый", rows, extras=dict(fonts, voice=True))
        high = [(b, char_for_byte(b)) for b in HIGH_BYTES]
        lower = [(b, chr(b)) for b in range(ord("a"), ord("z") + 1)]
        write_font2d(mods / args.name, font8x8, high)
        write_menufont(mods / args.name, font8x8, high + lower)
        write_voice(mods / args.name)
        tutorial = write_tutorial_voices(mods / args.name, game, levels)
        print("wrote %s (%d rows, fonts, voice, %d tutorial message tones)" % (mods / args.name, len(rows), tutorial))

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
