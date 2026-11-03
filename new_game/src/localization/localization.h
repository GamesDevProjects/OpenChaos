#ifndef LOCALIZATION_LOCALIZATION_H
#define LOCALIZATION_LOCALIZATION_H

// Localization mods: translated game text loaded from a mod folder, without
// replacing any game file.
//
// A mod is a folder localizations/<name>/ in the user data folder or in the
// install folder (the user folder wins when both have the same name). It holds
// mod.json (manifest) and a UTF-8 TSV mapping table keyed by the original
// English text. The engine keeps loading its English data; the hooks below
// swap each string for its translation while the data is loaded. The table is
// converted to the mod's single-byte codepage once, at startup.
//
// The active mod is chosen in the Options menu, or by
// "localization": { "mod": "<folder name>" } in OpenChaos.config.json, and
// applies after a restart.
//
// A mod can also bring font atlases for bytes >= 128 and a voice/ folder that
// overrides game audio and video files.
//
// Everything is gated by g_feature_flags.ext_localization_mods (off by
// default). With the flag off, or with no active mod, every hook returns its
// input untouched and no file is opened.

#include "engine/core/types.h" // CBYTE, UBYTE, SLONG, SWORD, ULONG

struct MissionData;

// Reads the config key, finds the mod, loads its table. Any problem prints one
// line to stderr and leaves the game in English. Call once at startup, after
// the config is loaded and before any UI text is read.
void LOC_startup(void);

// Rewrites the lang_*.txt string pool in place with the table's translations.
// Call at the end of XLAT_load (before XLAT_init).
void LOC_rewrite_lang_pool(void);

// Translates a mission title and briefing parsed from the .sty mission script.
void LOC_mission_data(MissionData* mdata);

// Returns the translation of an in-mission message (.ucm record or numbered
// .txt line), or the argument itself.
CBYTE* LOC_message(CBYTE* message);

// Returns the translation of a cutscene subtitle packet (may be NULL), or the
// argument itself.
CBYTE* LOC_cutscene_text(CBYTE* text);

// --- Strings OpenChaos added to the game ---

// Translation of a string drawn with FONT2D, or the argument itself. The lookup
// is by English text, one '\n'-separated line at a time, so a long help body is
// many short table rows. A line stays English if its translation has a byte the
// mod font cannot draw, or loses a {glyph} token.
// `english` must be a string literal: the result is cached by its address.
CBYTE* LOC_own(const char* english);
// The same for a string drawn with the menu font.
CBYTE* LOC_own_menu(const char* english);
// The same for a string shown as a panel message (CONSOLE_text_en), drawn with
// FONT2D. A translation longer than a panel message line stays English.
CBYTE* LOC_own_message(const char* english);

// --- Font for bytes >= 128 (loc_font.cpp) ---

// True if the mod's FONT2D map defines this byte. False for every byte < 128,
// and for every byte without a mod font.
bool LOC_font2d_mapped(CBYTE chr);
// Pen advance of a mapped byte in atlas pixels at scale 256, as FONT2D_GetLetterWidth.
SLONG LOC_font2d_advance(CBYTE chr);
// Draws a mapped byte from the mod atlas, returns the scaled pen advance, as FONT2D_DrawLetter.
SLONG LOC_font2d_draw(CBYTE chr, SLONG x, SLONG y, ULONG rgb, SLONG scale, SWORD fade);

// Puts the mod's menu glyphs into FontInfo. Safe to call repeatedly. Call after
// MENUFONT_MergeLower, which would overwrite the lowercase glyphs.
void LOC_menu_font_install(void);
// Puts the mod's glyphs for bytes >= 128 into FontInfo while it holds the alt
// font, which only has ASCII. Call after the alt font is swapped in.
void LOC_menu_font_alt_install(void);
// The POLY page to draw this menu glyph with: the mod page for a mapped byte,
// otherwise `page`. In the alt font only bytes >= 128 come from the mod.
SLONG LOC_menu_page(UBYTE ch, SLONG page);

// Uploads the mod's atlases. Call whenever texture pages have been rebuilt.
void LOC_font_on_textures_ready(void);

// The mod font pages are numbered below every other special page, but each one
// draws in the slot right after the game page it mirrors (FONT2D, NEWFONT_INVERSE,
// NEWFONT), so the HUD's translucent pages (speech bubbles) cannot cover mod
// letters. Independent of the feature flag: without a mod font these pages are empty.
// The page to draw at position `pos` of the index-ordered page loop: page index
// order with the mod pages moved. `pos` in 0..POLY_NUM_PAGES-1.
SLONG LOC_page_in_draw_order(SLONG pos);
// Moves the mod pages in a built PageOrder list to right after the pages they mirror.
void LOC_page_order_fix(SLONG* order, SLONG count);

// --- Media overlay ---

// Path of the mod's override for a game audio or video file, or `path` itself
// when there is none. The result stays valid until the next call. Without a
// voice folder in the mod this returns at once, with no file access.
char* LOC_media(const char* path);

// --- Discovery ---

// Discovered mods (valid subfolders of both localizations/ roots), sorted by
// folder name. The first call scans the folders; later calls reuse the list.
// Without the feature flag the list is empty and nothing is scanned.
int LOC_mod_count(void);
// Folder name, as stored in the config.
const char* LOC_mod_name(int index);
// The manifest's display_name, converted to the mod's codepage.
const char* LOC_mod_display_name(int index);

// Folder name of the mod loaded at startup, or "" when the game runs in English.
const char* LOC_active_mod(void);

// --- Language item of the Options menu (loc_menu.cpp) ---

// Adds the Language item and its restart note to the screen FRONTEND_easy just
// built, before its last row. Without the feature flag it does nothing.
void LOC_language_menu_add(void);
// Saves the choice to the config if the player changed it. Call when the screen
// is left.
void LOC_language_menu_store(void);

#endif // LOCALIZATION_LOCALIZATION_H
