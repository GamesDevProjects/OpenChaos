# Localization mods

Documented: 2026-11-01

A localization mod translates the game without replacing any of its files. It is a folder holding a mapping table keyed
by the original English text, and optionally a font for bytes 128 and up and voice or video overrides. The game keeps
loading its English data and swaps each string for its translation while loading.

The whole feature is behind the compile-time flag `ext_localization_mods`, off by default. With the flag off the game
behaves as if this page did not exist: no folder is scanned, no menu item appears, no string changes.

## Make a mod from scratch

This takes five minutes and gives a working mod. Nothing here needs a font, because the example translation is plain
ASCII.

1. **Build the game with the flag on.** Set `.ext_localization_mods = true` in `new_game/src/feature_flags.cpp` and
   rebuild.
2. **Create the folder** `localizations/mymod/` in the user data folder (`%APPDATA%\OpenChaos\` on Windows,
   `~/Library/Application Support/OpenChaos/` on macOS, `~/.local/share/OpenChaos/` on Linux).
3. **Write `mod.json`** in that folder, in UTF-8:

   ```json
   {
     "display_name": "Indonesia",
     "table": "strings.tsv",
     "codepage": "CP1251"
   }
   ```

4. **Write `strings.tsv`** next to it, in UTF-8. The separator between the two columns is a TAB:

   ```
   # English text <TAB> translation
   Options	Pilihan
   Sound	Suara
   Help	Bantuan
   ```

5. **Start the game.** Go to the main menu, then Options, then Options again. `Language` lists `English` and `Indonesia`.
   Pick `Indonesia` and leave the screen with Okay.
6. **Restart the game.** The menu now says `PILIHAN` and `SUARA`, and the pause menu says `BANTUAN`.

If nothing changes, read the game's `stderr`: `make r` writes it to `stderr.log` next to the exe, and from a terminal
`OpenChaos.exe 2> stderr.log` does the same. A loaded mod prints `localization: mymod loaded (3 rows, user folder)`.
Every problem prints one line starting with `localization:` and the game carries on in English.

To go further, get the rest of the keys (see "Where the keys come from"). For a non-Latin language, draw a font (see
"Fonts").

## Folder and discovery

Every subfolder of `localizations/` with a valid `mod.json` is one language. Two places are scanned: the user data
folder above, and the game's install folder.

- If both have a subfolder of the same name, the user folder wins, even if its copy is broken.
- A subfolder without a manifest, or with a broken one, is skipped with one line in `stderr`.
- The folder name is what the config stores. It must be a single path component of at most 64 bytes, with no `/`, `\`,
  `:` or `..`.

## Manifest

`mod.json` is UTF-8 JSON.

| Key | Required | Meaning |
|---|---|---|
| `display_name` | yes | The language's own name, e.g. `Русский`. Shown in the Language menu. |
| `table` | yes | File name of the mapping table, in the mod folder. |
| `codepage` | yes | The single-byte encoding the game draws with. `CP1251` (or `windows-1251`) is the only one supported today. |
| `font2d` | no | `{ "image": "font2d.tga", "map": "font2d.map" }`, the in-game text font. |
| `menufont` | no | `{ "image": "menufont.tga", "map": "menufont.map" }`, the menu font. |
| `voice` | no | `true` turns on the voice and video overlay. |

Other keys are ignored. File names are single path components, relative to the mod folder.

## The Language menu

Main menu, Options, Options: the screen with the scanner setting also has a `Language` row. Left and right, or confirm,
step through `English` (always first) and every discovered mod, sorted by folder name. Leaving the screen stores the
choice in `OpenChaos.config.json`:

```json
"localization": { "mod": "mymod" }
```

**The choice applies after a restart**, which the note under the row says. Nothing switches live. English stores an
empty string. Editing the file by hand works too, while the game is closed. A stored mod that no longer exists means
English, with one line in `stderr` and no dialog.

A `display_name` is drawn with the running mod's menu font, the only one loaded. If that font cannot draw it (always the
case for a Cyrillic name while the game runs in English), the row shows the folder name instead, so **give the folder a
readable ASCII name**, such as `russian`. The name shows properly once that mod is running.

## The mapping table

UTF-8 text, LF or CRLF line ends, one row per line. A line starting with `#` is a comment; blank lines are ignored.

```
key <TAB> value
key <TAB> value <TAB> context
```

- **key** is the English text, byte for byte as the game has it in memory.
- **value** is the translation. An empty value means "not translated yet", and the English text is used.
- **context** is optional and tells apart the rare sentence that needs two translations. A row with a context wins over a
  row without one; a row without one applies everywhere.

| Context | Applies to |
|---|---|
| `lang` | UI strings from `text/lang_*.txt` |
| `ucm`, `txt` | in-mission messages and cutscene subtitles (the two names are the same surface) |
| `sty` | mission titles and briefings |
| `own` | strings OpenChaos added to the game |

Escapes, in key and value: `\\`, `\t`, `\r`, `\n`, and `\xNN` for one raw byte (`NN` at least `20`). Everything else is
converted from UTF-8 to the mod's codepage once, at startup.

What happens to a bad row:

| Problem | Result |
|---|---|
| Bad escape, invalid UTF-8, no TAB, unknown context, control character, too many columns | **The whole mod is rejected**: one line in `stderr`, English |
| A character the codepage lacks | That row is ignored, with a warning |
| A value that does not keep the key's `\|N` tokens and `\|` separators | That row is ignored, with a warning |
| The same key twice with different values | The first wins, with one warning |
| The same key twice with the same value | Harmless |

`|N` is not an escape. It is a game control token that expands to a key name, as in `Press |16 to continue.`, and a `|`
alone separates the lines of a conversation. A value must carry exactly the same set of them, in any order.

The same English line can occur in many missions. One row translates every copy.

## Where the keys come from

| Text | Source | The key is |
|---|---|---|
| UI strings | `text/lang_english.txt` | the whole line, with its `\|N` tokens. The list ends at the first empty line. **The first line (`English`) must not be translated**: the game derives its language and the mission script's name from it, and a row for it is ignored. |
| Mission titles | `data/urban.sty`, the `Title` column | the title without the spaces around it |
| Briefings | `data/urban.sty`, the `Briefing` column | the briefing, with every `@` written as `\r` |
| Citizen and mission lines | `text/*.txt`, lines like `12. text` | the text after the number |
| In-mission messages | inside `levels/*.ucm` (binary) | the whole message record, with its `\|` separators |
| OpenChaos's own strings | see below | the line of English text |

`tools/localization_testmod/make_testmod.py` reads your copy of the game data and writes a table with the exact keys, so
it is the practical way to get the `.ucm` ones:

```
python tools/localization_testmod/make_testmod.py --game-dir <game folder> --name mymod --levels FTutor1.ucm,Assault1.ucm
```

It covers the given levels, the citizen files they name, their missions in `urban.sty`, and a sample of UI strings. The
values it writes are test filler, tagged `T<row>`: replace them. It also writes test fonts and a test voice file, and it
overwrites an existing `strings.tsv`, so point it at a scratch name and copy the rows you need. Add the rest of the UI
strings from `lang_english.txt` by hand.

## OpenChaos's own strings

Text OpenChaos added to the game goes through the same table, in context `own` or without a context:

| Text | Where | Drawn with |
|---|---|---|
| `Help` | pause menu item and help list heading | menu font |
| `CONTROLS`, `MOVEMENT`, `COMBAT`, `WEAPONS`, `DRIVING`, `OTHER` | help topic titles | menu font |
| every line of the help bodies | help screens, in `new_game/src/ui/menus/help_content_kbm.cpp`, `_xbox.cpp` and `_ps.cpp` | FONT2D font |
| `Language`, `Restart to apply` | Options menu | menu font |
| `Bonus mission unlocked!` | mission map (a hardcoded line of the original game, not in its data files) | FONT2D font |

- **Help bodies are translated line by line.** Each key is one line of the body, the text between two `\n` in the source,
  for example `{kb_space} - Jump.`. A long paragraph is one line. An untranslated line stays English, and a line changed
  in a later version falls back to English alone.
- **Keep the `{...}` tokens.** They are the button icons. A value must carry the same set, in any order, or that line
  stays English with a warning. Each input device has its own body, with its own tokens, so all three need rows.
- **A value with a byte the font lacks stays English**, with a warning. The check is against the font the string is drawn
  with, so a mod with only `font2d` gets its help text translated but not its help titles.
- Bytes 128 and up come from the mod font. Bytes below 128 keep the look these strings have without a mod, the
  replacement font on the help screens and the game's font elsewhere, so an untranslated string looks exactly as before.
  A translated help title that mixes Latin and non-Latin letters mixes the two fonts; prefer one script there.

`English` in the Language menu is not translatable: languages are listed under their own names, and this one has to stay
readable with any mod.

Not translatable, because they are not part of the game's normal text:

- The "files not found" and "language files not found" screens. They appear before a mod can load and draw with an
  embedded ASCII font.
- The "could not open the display" error dialog, for the same reason.
- The release-build `ASSERTION FAILURE` notice, the developer console and everything behind the debug keys.
- The OpenChaos block of the end credits, including its `OPENCHAOS` section title. The outro font has no support for
  bytes 128 and up, and its draw function, `FONT_draw`, takes each line as a `printf`-style format string.

## Fonts

Fonts work per byte. A byte the map defines draws from the mod's atlas; every other byte keeps the game's own glyph
and width, so Latin and translated text can mix on one line. A mod with no font changes nothing.

A broken font prints one line to `stderr` and is dropped: the table still works, and those bytes fall back to the
game's lookup. A row is broken when it has an unknown byte, an empty rectangle or a rectangle outside the image.

### In-game text: `font2d.tga` and `font2d.map`

- Image: uncompressed 32-bit TGA, up to 1024 pixels per side. Alpha is the coverage and RGB is white, because the
  text colour tints it. 256×256 is a good size.
- Map: one glyph per row, tabs or spaces between values, a line starting with `#` is a comment. Numbers are decimal,
  or hex with a `0x` prefix.

```
# byte   x   y   w   advance
0xC0     0   0   9   10
```

`x, y` is the top-left corner of the glyph cell in atlas pixels, `w` the ink width and `advance` the pen advance, both
in pixels. **The cell is always 18 pixels tall** and the baseline is its row 14, because the game draws every letter
with that fixed height. Leave a blank column between cells: the page uses linear filtering. Bytes `0x80`..`0xFF` only.

### Menu text: `menufont.tga` and `menufont.map`

- Image: uncompressed 32-bit TGA, same size limit. RGB is the intensity, because the page is additive. Alpha is the
  coverage, used by the shadow of menu titles, so it should match the intensity.
- Map: `byte x y w h`, where `w` and `h` are the glyph's size on screen at scale 256.

The pen advances by `w - 1`, as in the game's menu font, so the gap between letters has to be blank columns inside
`w`. Glyphs are aligned by their top edge, so use the height of the game's letters. Bytes `0x80`..`0xFF` and lowercase
`a`..`z`: the game's menu font has only capitals, and a map that defines lowercase gives the menus real lowercase.

## Voice and video overlay

With `"voice": true`, a file in the mod's `voice/` folder replaces the game file at the same relative path. A file the
mod lacks plays the original. Without `"voice": true` the game makes no extra file checks at all.

| Sound | Mod path |
|---|---|
| In-mission speech and briefings | `voice/talk2/...` |
| Barks | `voice/talk2/misc/...` |
| Cutscene and effect waves | `voice/data/sfx/1622/...` |
| Movies | `voice/bink/...` |

- WAV files must be **16-bit PCM**: the audio layer is given 16-bit data whatever the file holds.
- Each voice file is loaded whole into memory when it plays, so keep them small for handhelds.
- A file that cannot be decoded plays nothing, as with a broken game file.
- The file check uses the game's file layer. On Windows that layer cannot open paths outside the system codepage, so
  with a user folder such as `C:\Users\Иван` the overlay finds nothing and the originals play. A mod in the install
  folder is not affected.
- Message display time does not depend on the voice file. One-shot speech shows for a fixed time; a conversation line
  shows for a time derived from its length in bytes, and the next line waits for the voice to finish. A long voice
  file can outlast its text.

## Hard limits

The loader enforces these, sized to the fixed game buffers the texts are copied into. A row over a limit is refused
with one line in `stderr` and the English text is used.

| Text | Limit |
|---|---|
| One UI string, after key names are expanded | 359 bytes |
| "Get down" and "can't shoot a cop" prompts, shown as speech bubbles | 299 bytes |
| One line of an in-mission message (between `\|` separators) | 299 bytes |
| One whole message record, `\|` separators included | 1 023 bytes |
| One mission title, and the "empty" save slot label | 203 bytes |
| One briefing | 1 792 bytes |
| Yes + No, in total | 18 bytes |
| Camera + Character, in total | 253 bytes |
| All UI strings of the game | fits the 8 KB string pool |
| All messages of a level | fits the 16 KB message pool |

**`%` is not allowed in UI strings or in-mission messages.** The game passes them to `printf`-style functions as the
format string in some places and shows them verbatim in others, so neither `%` nor `%%` is right everywhere. No
English string contains one. A row with a `%` is refused; write the word out instead. The rule covers letterbox
subtitles, which are in-mission messages. Mission titles, briefings, OpenChaos's own strings and the subtitle packets
(`PT_TEXT`) of cutscene recordings, which are drawn verbatim, are not affected.

## Layout rules

These surfaces have fixed room and no shrink rule. The game does not check them, so they are rules for the translator.

| Surface | Room | Rule |
|---|---|---|
| Menu item labels | centred, no wrap; a label wider than 640 px is cut at both edges | ≤ 30 characters |
| Menu titles | no wrap; a long title runs off the screen | as short as possible |
| Menu multi-values | right-aligned, shrink to two lines by themselves | no limit |
| Save-slot labels | `"N: <mission title>"`, centred, no clip | mission titles ≤ 40 characters |
| Briefing text | wraps at 400 px, starts at y = 100, 15 px per line, no vertical limit | about 20 lines |
| In-mission speech bubbles | wrap, the stack grows upward, 8 messages at most | dialogue lines ≤ 255 bytes |
| Cutscene letterbox | 80 px bars, two lines fit; a third line is clipped by the bar | two lines |
| HUD words (ammo, prompts) | fixed panel art, no wrap | short words only |

## Test mod

`tools/localization_testmod/make_testmod.py` also generates a complete synthetic mod for checking the engine: Cyrillic
filler text on every surface, both fonts with a visible bar under each glyph, and a replaced bark. `--select NAME`
writes the config key, and `--broken` adds broken sibling folders for the discovery checks. Its header lists the
options. Nothing it writes goes into the repository: it reads the game data from your copy and writes to the user data
folder.
