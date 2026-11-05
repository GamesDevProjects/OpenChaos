// Localization mods: startup and the text hooks. See localization/localization.h.

#include "localization/localization.h"
#include "localization/loc_internal.h"

#include "feature_flags.h"
#include "engine/io/file.h" // FileExists
#include "engine/io/oc_config.h" // OC_CONFIG_get_string
#include "assets/xlat_str.h" // X_* ids, MAX_STRINGS, MAX_STRING_SPACE, XCTL_*
#include "assets/xlat_str_globals.h" // xlat_set, xlat_ptr, xlat_upto, xlat_buffer
#include "missions/eway_globals.h" // EWAY_mess_buffer_upto, EWAY_MESS_BUFFER_SIZE, talk_thing, EWAY_conv_talk
#include "missions/eway.h" // get_level_word
#include "engine/audio/mfx.h" // MFX_QUICK_stop, MFX_QUICK_play
#include "ui/frontend/frontend_globals.h" // MissionData, menu_buffer, menu_choice_*
#include "ui/hud/panel_globals.h" // PANEL_TEXT_MAX_LENGTH, PANEL_wide_text, PANEL_help_message

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

// --- Lang pool (text/lang_*.txt) ---------------------------------------------

// XLAT_load stores a "|N" token as this byte followed by the byte N.
// uc_orig: XLAT_compress_tokens (fallen/Source/xlat_str.cpp)
#define LOC_XLAT_TOKEN_BYTE 1

// Longest key name XLAT_str can put in place of a token: XLAT_init's default
// names, the longest being "shoulder".
// uc_orig: XLAT_init (fallen/Source/xlat_str.cpp)
#define LOC_XLAT_REMAP_NAME_MAX 8

// Pool bytes XLAT_init needs after the loaded strings: one name plus its
// terminator for each control id XCTL_JUMP (1) .. XCTL_CAM_CONTINUE.
#define LOC_XLAT_INIT_RESERVE (XCTL_CAM_CONTINUE * (LOC_XLAT_REMAP_NAME_MAX + 1))

// XLAT_str expands into xlat_buffer, so an expanded string must fit it.
static constexpr size_t LOC_LANG_EXPANDED_MAX = sizeof(xlat_buffer) - 1;

// Longest decimal text of an int ("-2147483648").
#define LOC_INT_TEXT_MAX 11

// Local CBYTE str[64] that the cones-hit line is built in.
// (missions/eway.cpp, EWAY_process_penalties)
#define LOC_CONES_LINE_SIZE 64
// sprintf "%s = %d": " = " + an int + terminator.
#define LOC_CONES_NUMBER_OVERHEAD (3 + LOC_INT_TEXT_MAX + 1)

// Local CBYTE str2[200] that the end-of-mission stats line is built in.
// (ui/frontend/frontend.cpp, FRONTEND_draw_districts)
#define LOC_STATS_LINE_SIZE 200
// "%s: %03d  %s: %03d  %s: %03d  %s: %03d\n": four ": " + three digits (the
// stats are UBYTE), three "  " separators, the newline and the terminator.
#define LOC_STATS_LINE_OVERHEAD (4 * (2 + 3) + 3 * 2 + 1 + 1)

// FRONTEND_find_savegames lists this many save slots back to back in menu_buffer.
// Each label is a saved mission title or X_EMPTY, with its terminator.
#define LOC_SAVE_SLOT_COUNT 10
static constexpr size_t LOC_SAVE_LABEL_SIZE = sizeof(menu_buffer) / LOC_SAVE_SLOT_COUNT;

// Lang strings that the game copies together into one fixed buffer. If their
// translations would not fit, the whole group stays English.
struct LocLangGroup {
    int ids[4];
    int count;
    size_t overhead; // terminators and format text around the strings
    size_t capacity; // size of the buffer they are copied into
    const char* what;
};

static const LocLangGroup LOC_LANG_GROUPS[] = {
    // frontend.cpp FRONTEND_init: X_NO and X_YES copied back to back.
    { { X_NO, X_YES }, 2, 2, sizeof(menu_choice_yesno), "Yes/No choice" },
    // frontend.cpp FRONTEND_init: X_CAMERA and X_CHARACTER copied back to back.
    { { X_CAMERA, X_CHARACTER }, 2, 2, sizeof(menu_choice_scanner), "scanner choice" },
    { { X_NO_CONES_HIT }, 1, 1, LOC_CONES_LINE_SIZE, "cones line" },
    { { X_NUM_CONES_HIT }, 1, LOC_CONES_NUMBER_OVERHEAD, LOC_CONES_LINE_SIZE, "cones count line" },
    { { X_CON_INCREASED, X_STA_INCREASED, X_STR_INCREASED, X_REF_INCREASED }, 4, LOC_STATS_LINE_OVERHEAD,
        LOC_STATS_LINE_SIZE, "stats line" },
    // person.cpp: the only UI strings shown as speech bubbles. PANEL_new_text
    // drops text that does not fit its queue.
    { { X_GET_DOWN }, 1, 1, PANEL_TEXT_MAX_LENGTH, "speech bubble" },
    { { X_CANT_SHOOT_COP }, 1, 1, PANEL_TEXT_MAX_LENGTH, "speech bubble" },
    { { X_EMPTY }, 1, 1, LOC_SAVE_LABEL_SIZE, "empty save slot label" },
};

#define LOC_LANG_GROUP_COUNT (sizeof(LOC_LANG_GROUPS) / sizeof(LOC_LANG_GROUPS[0]))

// --- Messages, titles, briefings ----------------------------------------------

// One displayed message line must fit every panel buffer it can be copied
// into: PANEL_new_text drops text of PANEL_TEXT_MAX_LENGTH or more, and the
// cutscene letterbox and help message copy it into fixed arrays.
static constexpr size_t LOC_MESSAGE_LINE_MAX
    = std::min({ (size_t)PANEL_TEXT_MAX_LENGTH, sizeof(PANEL_wide_text), sizeof(PANEL_help_message) }) - 1;

// A whole message record, '|' separators included, passes through the local
// CBYTE message[1024] of PANEL_new_text when it is shown as one piece.
#define LOC_PANEL_FORMAT_BUFFER_SIZE 1024
static constexpr size_t LOC_MESSAGE_RECORD_MAX = LOC_PANEL_FORMAT_BUFFER_SIZE - 1;

// A mission title is stored in MissionData::ttl, and a save slot's label is the
// title of the saved mission.
static constexpr size_t LOC_TITLE_MAX = std::min(sizeof(MissionData::ttl), LOC_SAVE_LABEL_SIZE) - 1;
// The briefing and the title are copied together into menu_buffer
// (FRONTEND_MissionBrief): briefing + terminator + title + terminator.
static constexpr size_t LOC_BRIEF_MAX
    = std::min(sizeof(MissionData::brief) - 1, sizeof(menu_buffer) - sizeof(MissionData::ttl) - 1);

// --- Media overlay -------------------------------------------------------------

// Folder inside a mod that mirrors the game's audio and video paths.
#define LOC_VOICE_DIR "voice"
// Size of the path buffers in FILE_resolve_ci, which every media path passes through.
#define LOC_MEDIA_PATH_SIZE 512
// Buffer for get_level_word, the size EWAY_talk gives it.
// uc_orig: EWAY_talk (fallen/Source/eway.cpp)
#define LOC_LEVEL_WORD_SIZE 100

// --- State ---------------------------------------------------------------------

static bool s_started = false;
static bool s_active = false;
static std::string s_active_name;
static LocTable s_table;
static std::string s_voice_dir; // empty: no media overlay, nothing is probed

static bool s_pool_overflow_logged = false;
static bool s_group_logged[LOC_LANG_GROUP_COUNT] = {};
static bool s_message_pool_logged = false;

// Where an own string is drawn. Each place has its own cache, because each font
// can draw different bytes and a panel message has a length limit.
enum LocOwnUse {
    LOC_OWN_FONT2D,
    LOC_OWN_MENU,
    LOC_OWN_MESSAGE, // FONT2D, through the message panel
    LOC_OWN_USE_COUNT
};

// Translated own strings by the address of their English literal. An empty value
// means nothing on that string was translated. Only as many entries as there are
// literals.
static std::unordered_map<const char*, std::string> s_own_cache[LOC_OWN_USE_COUNT];

void LOC_startup(void)
{
    if (!g_feature_flags.ext_localization_mods || s_started)
        return;
    s_started = true;

    const std::string config_value = OC_CONFIG_get_string(LOC_CONFIG_SECTION, LOC_CONFIG_KEY, "");
    const std::string name(LOC_trim(config_value));
    if (name.empty())
        return; // no mod selected: English

    const char* why;
    if (!LOC_check_name(name, &why)) {
        fprintf(stderr, LOC_LOG_PREFIX "mod name '%s' rejected (%s), using English\n", name.c_str(), why);
        return;
    }

    const LocModInfo* mod = nullptr;
    for (const LocModInfo& m : LOC_discover()) {
        if (LOC_same_name(m.name, name)) {
            mod = &m;
            break;
        }
    }
    if (!mod) {
        // A folder that discovery skipped has already printed its reason.
        if (!LOC_discovery_skipped(name))
            fprintf(stderr, LOC_LOG_PREFIX "mod '%s' not found in localizations/, using English\n", name.c_str());
        return;
    }

    std::string error;
    if (!LOC_load_table(mod->dir / LOC_path_from_utf8(mod->table_file), *mod->codepage, s_table, error)) {
        s_table.clear();
        fprintf(stderr, LOC_LOG_PREFIX "'%s' rejected: %s, using English\n", mod->name.c_str(), error.c_str());
        return;
    }

    s_active = true;
    s_active_name = mod->name;
    fprintf(stderr, LOC_LOG_PREFIX "%s loaded (%d rows, %s folder)\n", mod->name.c_str(), s_table.rows,
        mod->user_folder ? "user" : "install");

    LOC_load_fonts(*mod);

    if (mod->voice) {
        std::error_code ec;
        const std::filesystem::path voice = mod->dir / LOC_VOICE_DIR;
        if (std::filesystem::is_directory(voice, ec))
            s_voice_dir = LOC_path_to_utf8(voice);
        else
            fprintf(stderr, LOC_LOG_PREFIX "%s: the manifest enables voice but there is no " LOC_VOICE_DIR " folder\n",
                mod->name.c_str());
    }
}

char* LOC_media(const char* path)
{
    if (s_voice_dir.empty())
        return (char*)path;

    // Engine paths are relative, some with a leading "./".
    const char* rel = path;
    if (rel[0] == '.' && (rel[1] == '/' || rel[1] == '\\'))
        rel += 2;

    static CBYTE media_path[LOC_MEDIA_PATH_SIZE];
    const int len = snprintf(media_path, sizeof(media_path), "%s/%s", s_voice_dir.c_str(), rel);
    if (len < 0 || len >= (int)sizeof(media_path))
        return (char*)path;
    for (CBYTE* p = media_path; *p; p++) {
        if (*p == '\\')
            *p = '/';
    }
    return FileExists(media_path) ? media_path : (char*)path;
}

const char* LOC_video_audio(const char* video_path)
{
    if (s_voice_dir.empty())
        return nullptr;

    // Sound containers a replacement track may come in, in lookup order.
    static const char* const TRACK_EXTENSIONS[] = { ".ogg", ".wav" };

    // The extension is the part after the last '.' of the file name, not of a folder.
    size_t stem_len = strlen(video_path);
    for (size_t i = stem_len; i-- > 0;) {
        if (video_path[i] == '/' || video_path[i] == '\\')
            break;
        if (video_path[i] == '.') {
            stem_len = i;
            break;
        }
    }

    static CBYTE track_path[LOC_MEDIA_PATH_SIZE];
    for (const char* ext : TRACK_EXTENSIONS) {
        CBYTE game_path[LOC_MEDIA_PATH_SIZE];
        const int len = snprintf(game_path, sizeof(game_path), "%.*s%s", (int)stem_len, video_path, ext);
        if (len < 0 || len >= (int)sizeof(game_path))
            return nullptr;
        // LOC_media returns its argument when the mod has no such file: a
        // track in the game folder is not a mod track.
        const char* found = LOC_media(game_path);
        if (found != game_path) {
            snprintf(track_path, sizeof(track_path), "%s", found);
            return track_path;
        }
    }
    return nullptr;
}

// Plays a voice line the original game never voices, cutting off any voice
// line still playing, if its file exists (in the mod's voice folder or the
// game's). Without the file nothing happens: no voice and no cut-off line.
// Returns whether the line started.
static bool loc_play_line(CBYTE* path)
{
    if (!FileExists(LOC_media(path)))
        return false;
    talk_thing = NULL; // the line has no speaker
    MFX_QUICK_stop();
    MFX_QUICK_play(path); // applies the mod overlay itself
    EWAY_conv_talk = 0; // as EWAY_talk: the voice slot no longer holds a conversation line
    return true;
}

void LOC_tutorial_talk(ULONG sample)
{
    // The same file name EWAY_talk builds for a spoken message.
    CBYTE level[LOC_LEVEL_WORD_SIZE];
    CBYTE path[LOC_MEDIA_PATH_SIZE];
    get_level_word(level);
    const int len = snprintf(path, sizeof(path), "./talk2/%s.ucm%u.wav", level, (unsigned)sample);
    if (len < 0 || len >= (int)sizeof(path))
        return;
    // Not EWAY_talk: its MFX_QUICK_wait would freeze the game until the
    // previous line ends, and the next tutorial message often comes while it
    // still plays. The line has no speaker, so nothing that happens to a
    // character stops it; it plays out after the player closes the message.
    loc_play_line(path);
}

// The civilian casualty warnings after a won mission: X_CIVSDEAD_WARNING_1 ..
// X_CIVSDEAD_WARNING_4. The fourth is shown for every warning past the third.
// (game/game.cpp, the GS_LEVEL_WON branch of the level loop)
#define LOC_CIV_WARNING_COUNT (X_CIVSDEAD_WARNING_4 - X_CIVSDEAD_WARNING_1 + 1)

// Whether LOC_civ_warning_talk started a line that LOC_civ_warning_hush must stop.
static bool s_civ_warning_playing = false;

void LOC_civ_warning_talk(SLONG warning)
{
    s_civ_warning_playing = false;
    if (warning < 0)
        return;
    const int number = std::min((int)warning, LOC_CIV_WARNING_COUNT - 1) + 1;

    CBYTE path[LOC_MEDIA_PATH_SIZE];
    const int len = snprintf(path, sizeof(path), "./talk2/civsdead_warning%d.wav", number);
    if (len < 0 || len >= (int)sizeof(path))
        return;
    s_civ_warning_playing = loc_play_line(path);
}

void LOC_civ_warning_hush(void)
{
    if (!s_civ_warning_playing)
        return;
    s_civ_warning_playing = false;
    MFX_QUICK_stop();
}

// Translation of `key` for a surface: a row with that context first, then a
// row without context. nullptr if none.
static LocEntry* loc_find(std::string_view key, LocSurface surface)
{
    auto it = s_table.map[surface].find(key);
    if (it != s_table.map[surface].end())
        return &it->second;
    it = s_table.map[LOC_SURFACE_ANY].find(key);
    if (it != s_table.map[LOC_SURFACE_ANY].end())
        return &it->second;
    return nullptr;
}

// Logs once per row and surface that a translation is too long for its buffer.
static void loc_refuse(LocEntry& e, LocSurface surface, const char* where, size_t len, size_t max)
{
    const uint8_t bit = (uint8_t)(1u << surface);
    if (e.refused_logged & bit)
        return;
    e.refused_logged |= bit;
    fprintf(stderr, LOC_LOG_PREFIX "table row %d: too long for %s (%zu > %zu bytes), English used\n", e.row, where,
        len, max);
}

// The game passes UI strings and messages to PANEL_new_text, PANEL_new_help_message
// and PANEL_new_info_message as printf format strings, and shows some of them
// verbatim, so neither a bare '%' nor "%%" is right everywhere. No English string
// contains a '%', so a translation with one is refused: true means refused.
static bool loc_refuse_percent(LocEntry& e, LocSurface surface)
{
    if (e.value.find('%') == std::string::npos)
        return false;
    const uint8_t bit = (uint8_t)(1u << surface);
    if (!(e.refused_logged & bit)) {
        e.refused_logged |= bit;
        fprintf(stderr, LOC_LOG_PREFIX "table row %d: '%%' is not allowed in this text, English used\n", e.row);
    }
    return true;
}

// Pool string (tokens compressed) -> table key form ("|N").
static std::string loc_lang_unpack(const CBYTE* s)
{
    std::string out;
    for (; *s; s++) {
        if (*s == LOC_XLAT_TOKEN_BYTE && s[1]) {
            out += '|';
            out += std::to_string((UBYTE)s[1]);
            s++;
        } else {
            out += *s;
        }
    }
    return out;
}

// Table value -> pool string, compressing "|N" the way XLAT_load does.
static std::string loc_lang_pack(const std::string& v)
{
    std::string out;
    for (size_t i = 0; i < v.size(); i++) {
        size_t j = i + 1;
        if (v[i] == '|') {
            while (j < v.size() && v[j] >= '0' && v[j] <= '9')
                j++;
        }
        if (v[i] != '|' || j == i + 1) {
            out += v[i];
            continue;
        }
        out += (char)LOC_XLAT_TOKEN_BYTE;
        out += (char)atoi(v.substr(i + 1, j - i - 1).c_str());
        i = j - 1;
    }
    return out;
}

// Upper bound of XLAT_str's output length for a pool string.
static size_t loc_lang_expanded_len(const std::string& packed)
{
    size_t len = 0;
    for (size_t i = 0; i < packed.size(); i++) {
        if (packed[i] == LOC_XLAT_TOKEN_BYTE && i + 1 < packed.size()) {
            len += LOC_XLAT_REMAP_NAME_MAX;
            i++;
        } else {
            len++;
        }
    }
    return len;
}

void LOC_rewrite_lang_pool(void)
{
    if (!s_active)
        return;

    std::string chosen[MAX_STRINGS];
    bool translated[MAX_STRINGS] = {};
    bool any = false;

    for (int id = 0; id < MAX_STRINGS; id++) {
        if (!xlat_ptr[id])
            continue;
        chosen[id] = xlat_ptr[id];
        // Line 0 names the language; the game derives IsEnglish and the
        // mission script name from it, so it must stay English.
        if (id == X_THIS_LANGUAGE_IS)
            continue;
        LocEntry* e = loc_find(loc_lang_unpack(xlat_ptr[id]), LOC_SURFACE_LANG);
        if (!e || loc_refuse_percent(*e, LOC_SURFACE_LANG))
            continue;
        std::string packed = loc_lang_pack(e->value);
        const size_t expanded = loc_lang_expanded_len(packed);
        if (expanded > LOC_LANG_EXPANDED_MAX) {
            loc_refuse(*e, LOC_SURFACE_LANG, "a UI string", expanded, LOC_LANG_EXPANDED_MAX);
            continue;
        }
        chosen[id] = std::move(packed);
        translated[id] = true;
        any = true;
    }
    if (!any)
        return;

    for (size_t g = 0; g < LOC_LANG_GROUP_COUNT; g++) {
        const LocLangGroup& grp = LOC_LANG_GROUPS[g];
        size_t total = grp.overhead;
        bool group_translated = false;
        for (int k = 0; k < grp.count; k++) {
            total += loc_lang_expanded_len(chosen[grp.ids[k]]);
            group_translated |= translated[grp.ids[k]];
        }
        if (!group_translated || total <= grp.capacity)
            continue;
        for (int k = 0; k < grp.count; k++) {
            const int id = grp.ids[k];
            if (xlat_ptr[id])
                chosen[id] = xlat_ptr[id];
            translated[id] = false;
        }
        if (!s_group_logged[g]) {
            s_group_logged[g] = true;
            fprintf(stderr, LOC_LOG_PREFIX "%s too long (%zu > %zu bytes), English used\n", grp.what, total,
                grp.capacity);
        }
    }

    size_t total = LOC_XLAT_INIT_RESERVE;
    for (int id = 0; id < MAX_STRINGS; id++) {
        if (xlat_ptr[id])
            total += chosen[id].size() + 1;
    }
    if (total > MAX_STRING_SPACE) {
        if (!s_pool_overflow_logged) {
            s_pool_overflow_logged = true;
            fprintf(stderr, LOC_LOG_PREFIX "UI strings need %zu bytes, the pool holds %d; English used\n", total,
                MAX_STRING_SPACE);
        }
        return;
    }

    // `chosen` holds copies, so the pool can be rebuilt from scratch.
    memset(xlat_set, 0, sizeof(xlat_set));
    CBYTE* upto = xlat_set;
    for (int id = 0; id < MAX_STRINGS; id++) {
        if (!xlat_ptr[id])
            continue;
        memcpy(upto, chosen[id].c_str(), chosen[id].size() + 1);
        xlat_ptr[id] = upto;
        upto += chosen[id].size() + 1;
    }
    xlat_upto = upto;
}

// Longest '|'-separated line of a message (conversations are split at '|'
// when shown).
static size_t loc_longest_line(const std::string& s)
{
    size_t longest = 0, cur = 0;
    for (char c : s) {
        if (c == '|') {
            longest = std::max(longest, cur);
            cur = 0;
        } else {
            cur++;
        }
    }
    return std::max(longest, cur);
}

CBYTE* LOC_message(CBYTE* message)
{
    if (!s_active || !message)
        return message;
    LocEntry* e = loc_find(message, LOC_SURFACE_MESSAGE);
    if (!e || loc_refuse_percent(*e, LOC_SURFACE_MESSAGE))
        return message;

    const size_t line = loc_longest_line(e->value);
    if (line > LOC_MESSAGE_LINE_MAX) {
        loc_refuse(*e, LOC_SURFACE_MESSAGE, "a message line", line, LOC_MESSAGE_LINE_MAX);
        return message;
    }
    if (e->value.size() > LOC_MESSAGE_RECORD_MAX) {
        loc_refuse(*e, LOC_SURFACE_MESSAGE, "a message", e->value.size(), LOC_MESSAGE_RECORD_MAX);
        return message;
    }
    // The level's message pool drops a message that does not fit. Never let a
    // longer translation cost a message that fits in English.
    const size_t room = (size_t)(EWAY_MESS_BUFFER_SIZE - EWAY_mess_buffer_upto);
    if (e->value.size() + 1 > room && strlen(message) + 1 <= room) {
        if (!s_message_pool_logged) {
            s_message_pool_logged = true;
            fprintf(stderr, LOC_LOG_PREFIX "level message pool full (%d bytes), English used for the rest\n",
                EWAY_MESS_BUFFER_SIZE);
        }
        return message;
    }
    return (CBYTE*)e->value.c_str();
}

CBYTE* LOC_cutscene_text(CBYTE* text)
{
    if (!s_active || !text)
        return text;
    LocEntry* e = loc_find(text, LOC_SURFACE_MESSAGE);
    if (!e)
        return text;
    if (e->value.size() > LOC_MESSAGE_LINE_MAX) {
        loc_refuse(*e, LOC_SURFACE_MESSAGE, "a message line", e->value.size(), LOC_MESSAGE_LINE_MAX);
        return text;
    }
    return (CBYTE*)e->value.c_str();
}

// Replaces buf with its translation if one exists and fits. The key is buf
// without the trailing characters in `trim`.
static void loc_translate_in_place(CBYTE* buf, size_t max_len, const char* trim, const char* where)
{
    std::string_view key(buf);
    while (!key.empty() && strchr(trim, key.back()))
        key.remove_suffix(1);
    LocEntry* e = loc_find(key, LOC_SURFACE_STY);
    if (!e)
        return;
    if (e->value.size() > max_len) {
        loc_refuse(*e, LOC_SURFACE_STY, where, e->value.size(), max_len);
        return;
    }
    memcpy(buf, e->value.c_str(), e->value.size() + 1);
}

void LOC_mission_data(MissionData* mdata)
{
    if (!s_active)
        return;
    // Titles come out of the script with the padding before the next ':'.
    loc_translate_in_place(mdata->ttl, LOC_TITLE_MAX, " ", "a mission title");
    // A briefing keeps its line end when the script has LF line ends.
    loc_translate_in_place(mdata->brief, LOC_BRIEF_MAX, "\r\n", "a briefing");
}

// The {glyph} tokens of a help line, sorted. Help lines draw them as button icons.
static std::vector<std::string_view> loc_glyph_tokens(std::string_view s)
{
    std::vector<std::string_view> tokens;
    for (size_t open = s.find('{'); open != std::string_view::npos; open = s.find('{', open + 1)) {
        const size_t close = s.find('}', open);
        if (close == std::string_view::npos)
            break;
        tokens.push_back(s.substr(open, close - open + 1));
    }
    std::sort(tokens.begin(), tokens.end());
    return tokens;
}

// Logs once per row why an own string stays English.
static void loc_own_refuse(LocEntry& e, const char* why)
{
    const uint8_t bit = (uint8_t)(1u << LOC_SURFACE_OWN);
    if (e.refused_logged & bit)
        return;
    e.refused_logged |= bit;
    fprintf(stderr, LOC_LOG_PREFIX "table row %d: %s, English used\n", e.row, why);
}

// The string with each translated line replaced, or "" if no line was translated.
// A result longer than max_len bytes is "" too.
static std::string loc_own_translate(std::string_view english, bool menu, size_t max_len)
{
    std::string out;
    bool any = false;
    size_t pos = 0;
    while (true) {
        const size_t eol = english.find('\n', pos);
        const std::string_view line = english.substr(pos, eol == std::string_view::npos ? eol : eol - pos);
        LocEntry* e = line.empty() ? nullptr : loc_find(line, LOC_SURFACE_OWN);
        bool usable = e != nullptr;
        if (e && loc_glyph_tokens(line) != loc_glyph_tokens(e->value)) {
            loc_own_refuse(*e, "the value does not keep the key's {glyph} tokens");
            usable = false;
        }
        if (usable && !LOC_font_covers(e->value, menu)) {
            loc_own_refuse(*e, "the mod font lacks a character of this text");
            usable = false;
        }
        if (usable && e->value.size() > max_len) {
            loc_refuse(*e, LOC_SURFACE_OWN, "a message line", e->value.size(), max_len);
            usable = false;
        }
        out += usable ? std::string_view(e->value) : line;
        any |= usable;
        if (eol == std::string_view::npos)
            break;
        out += '\n';
        pos = eol + 1;
    }
    if (out.size() > max_len)
        return std::string(); // several lines that fit alone but not together
    return any ? out : std::string();
}

static CBYTE* loc_own(const char* english, LocOwnUse use)
{
    if (!s_active || !english)
        return (CBYTE*)english;
    auto& cache = s_own_cache[use];
    auto it = cache.find(english);
    if (it == cache.end()) {
        const size_t max_len = (use == LOC_OWN_MESSAGE) ? LOC_MESSAGE_LINE_MAX : std::string::npos;
        it = cache.emplace(english, loc_own_translate(english, use == LOC_OWN_MENU, max_len)).first;
    }
    return it->second.empty() ? (CBYTE*)english : (CBYTE*)it->second.c_str();
}

CBYTE* LOC_own(const char* english)
{
    return loc_own(english, LOC_OWN_FONT2D);
}

CBYTE* LOC_own_menu(const char* english)
{
    return loc_own(english, LOC_OWN_MENU);
}

CBYTE* LOC_own_message(const char* english)
{
    return loc_own(english, LOC_OWN_MESSAGE);
}

int LOC_mod_count(void)
{
    if (!g_feature_flags.ext_localization_mods)
        return 0;
    return (int)LOC_discover().size();
}

const char* LOC_mod_name(int index)
{
    if (index < 0 || index >= LOC_mod_count())
        return "";
    return LOC_discover()[index].name.c_str();
}

const char* LOC_mod_display_name(int index)
{
    if (index < 0 || index >= LOC_mod_count())
        return "";
    return LOC_discover()[index].display_name.c_str();
}

const char* LOC_active_mod(void)
{
    return s_active_name.c_str();
}
