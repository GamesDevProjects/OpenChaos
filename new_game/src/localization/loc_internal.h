#ifndef LOCALIZATION_LOC_INTERNAL_H
#define LOCALIZATION_LOC_INTERNAL_H

// Types and functions shared by the localization module's own files.
// Game code includes localization/localization.h only.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Every log line of the module starts with this.
#define LOC_LOG_PREFIX "localization: "

// Config location of the chosen mod's folder name.
#define LOC_CONFIG_SECTION "localization"
#define LOC_CONFIG_KEY "mod"

// UTF-8 byte order mark some editors write at the start of a text file.
inline constexpr char LOC_UTF8_BOM[] = "\xEF\xBB\xBF";

// Which engine text a table row applies to (the optional third TSV column).
enum LocSurface : uint8_t {
    LOC_SURFACE_ANY = 0, // no context column: the row applies everywhere
    LOC_SURFACE_LANG, // "lang": text/lang_*.txt UI strings
    LOC_SURFACE_MESSAGE, // "ucm" or "txt": in-mission messages and cutscene subtitles
    LOC_SURFACE_STY, // "sty": mission titles and briefings
    LOC_SURFACE_OWN, // "own": strings OpenChaos added to the game
    LOC_SURFACE_COUNT
};

// A single-byte target codepage: the Unicode code point of each byte
// 0x80..0xFF (0 = byte not defined). Bytes below 0x80 are ASCII.
struct LocCodepage {
    const char* name;
    const char* alias;
    const uint16_t* high;
};

// Number of entries in LocCodepage::high (bytes 0x80..0xFF).
#define LOC_CODEPAGE_HIGH_COUNT 128
// Code point range for the 7-bit ASCII identity part of every codepage.
#define LOC_ASCII_LIMIT 0x80

// One translation, already converted to the mod's codepage.
struct LocEntry {
    std::string value;
    int row = 0; // table line number, for log messages
    uint8_t refused_logged = 0; // bit (1 << LocSurface): a size refusal was already logged
    bool dup_warned = false; // a conflicting duplicate was already reported
};

// Hash that accepts std::string_view, so lookups need no allocation.
struct LocStringHash {
    using is_transparent = void;
    size_t operator()(std::string_view s) const noexcept { return std::hash<std::string_view> {}(s); }
};

using LocMap = std::unordered_map<std::string, LocEntry, LocStringHash, std::equal_to<>>;

struct LocTable {
    LocMap map[LOC_SURFACE_COUNT];
    int rows = 0; // rows accepted
    void clear()
    {
        for (LocMap& m : map)
            m.clear();
        rows = 0;
    }
};

// Files of one optional font, as named by the manifest.
struct LocFontFiles {
    bool declared = false; // the manifest has an entry for this font
    std::string image; // UTF-8 file names, empty if the entry lacks them
    std::string map;
};

// A valid mod found by discovery.
struct LocModInfo {
    std::string name; // folder name (UTF-8)
    std::string display_name; // manifest display_name in the mod's codepage
    std::filesystem::path dir;
    std::string table_file; // table file name from the manifest (UTF-8)
    const LocCodepage* codepage = nullptr;
    bool user_folder = false; // found in the user data folder (else the install)
    LocFontFiles font2d;
    LocFontFiles menufont;
    bool voice = false; // the manifest enables the media overlay
};

// --- loc_table.cpp ---

// Codepage by manifest name (case-insensitive), or nullptr if unsupported.
const LocCodepage* LOC_find_codepage(std::string_view name);

// UTF-8 -> codepage for short labels: invalid or unmappable characters become '?'.
std::string LOC_utf8_to_codepage_lossy(std::string_view utf8, const LocCodepage& cp);

// Parses a mapping table. On a syntax error returns false with a one-line
// reason in `error` and prints nothing. On success prints the row-level
// warnings (bad |N tokens, unmappable characters, conflicting duplicates).
bool LOC_load_table(const std::filesystem::path& file, const LocCodepage& cp, LocTable& table, std::string& error);

// Reads a whole file into `out`. False if it cannot be read or exceeds max_bytes.
bool LOC_read_file(const std::filesystem::path& file, size_t max_bytes, std::string& out);

// `s` without leading and trailing spaces and tabs.
std::string_view LOC_trim(std::string_view s);

// --- loc_font.cpp ---

// Loads the fonts the manifest declares. A broken font prints one line and is
// dropped: the table still works, and bytes >= 128 keep the game's lookup.
void LOC_load_fonts(const LocModInfo& mod);

// True if the loaded mod font can draw every byte >= 128 of `text`: the menu font
// if `menu`, otherwise the FONT2D font. Bytes below 128 always count as drawable.
bool LOC_font_covers(std::string_view text, bool menu);

// --- loc_config.cpp ---

// A mod or table name must be one plain path component. False + reason if not.
bool LOC_check_name(std::string_view name, const char** reason);

// Discovered valid mods (scans once per session, logs skipped folders once).
const std::vector<LocModInfo>& LOC_discover(void);

// True if discovery saw a folder of this name but skipped every copy of it
// (the reason was already logged).
bool LOC_discovery_skipped(std::string_view name);

// Case-insensitive (ASCII) name comparison, as folder names are on Windows.
bool LOC_same_name(std::string_view a, std::string_view b);

// Path from a UTF-8 string (std::filesystem would use the ANSI codepage on Windows).
std::filesystem::path LOC_path_from_utf8(std::string_view s);

// UTF-8 string of a path, for log lines.
std::string LOC_path_to_utf8(const std::filesystem::path& p);

#endif // LOCALIZATION_LOC_INTERNAL_H
