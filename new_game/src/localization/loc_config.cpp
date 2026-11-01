// Localization mods: name rules, manifest and discovery of the mod folders.
//
// Mods live in localizations/<name>/ under two roots: the user data folder
// (engine/io/user_data.h) and the install folder (the working directory, as
// for the game data). The file layer cannot list folders, so both roots are
// scanned here with std::filesystem. A name present in both roots is taken
// from the user folder. A subfolder without a usable mod.json is skipped
// with one stderr line; a broken copy in the user folder makes that name
// unavailable, the install copy is not used instead.
//
// mod.json:
//     {
//       "display_name": "<the language's own name, UTF-8>",   required
//       "table": "strings.tsv",                               required
//       "codepage": "CP1251",                                 required
//       "font2d": { "image": "font2d.tga", "map": "font2d.map" },        optional
//       "menufont": { "image": "menufont.tga", "map": "menufont.map" },  optional
//       "voice": true                                         optional
//     }
// Other keys (name, version) are ignored. Font files are checked when the mod
// is loaded, so a broken font does not hide the mod.

#include "localization/loc_internal.h"
#include "engine/io/user_data.h" // USERDATA_root

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>

namespace fs = std::filesystem;

// Folder holding the mods, under each root.
#define LOC_MODS_DIR "localizations"
// Manifest file name inside a mod folder.
#define LOC_MANIFEST_FILE "mod.json"
// A manifest is a few lines; anything bigger is not a manifest.
#define LOC_MANIFEST_MAX_BYTES (64 * 1024)
// Longest accepted mod or table name, in bytes. Folder names are short
// language names; the limit keeps log lines and config values sane.
#define LOC_NAME_MAX_BYTES 64

static std::vector<LocModInfo> s_mods;
static std::vector<std::string> s_skipped;
static bool s_discovered = false;

fs::path LOC_path_from_utf8(std::string_view s)
{
    return fs::path(std::u8string((const char8_t*)s.data(), s.size()));
}

std::string LOC_path_to_utf8(const fs::path& p)
{
    const std::u8string u = p.generic_u8string();
    return std::string((const char*)u.data(), u.size());
}

bool LOC_same_name(std::string_view a, std::string_view b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); i++) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z')
            ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z')
            cb = (char)(cb - 'A' + 'a');
        if (ca != cb)
            return false;
    }
    return true;
}

bool LOC_check_name(std::string_view name, const char** reason)
{
    if (name.empty()) {
        *reason = "empty name";
        return false;
    }
    if (name.size() > LOC_NAME_MAX_BYTES) {
        *reason = "name too long";
        return false;
    }
    if (name == "." || name.find("..") != std::string_view::npos) {
        *reason = "name contains '..'";
        return false;
    }
    for (char c : name) {
        if (c == '/' || c == '\\' || c == ':') {
            *reason = "name contains a path separator or a drive colon";
            return false;
        }
        if ((unsigned char)c < 0x20) {
            *reason = "name contains a control character";
            return false;
        }
    }
    return true;
}

// Reads and validates dir/mod.json. False + reason if the mod is unusable.
static bool read_manifest(const fs::path& dir, LocModInfo& info, std::string& reason)
{
    std::error_code ec;
    const fs::path manifest = dir / LOC_MANIFEST_FILE;
    if (!fs::is_regular_file(manifest, ec)) {
        reason = "no " LOC_MANIFEST_FILE;
        return false;
    }
    std::string text;
    if (!LOC_read_file(manifest, LOC_MANIFEST_MAX_BYTES, text)) {
        reason = "cannot read " LOC_MANIFEST_FILE;
        return false;
    }
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        reason = LOC_MANIFEST_FILE " is not valid JSON";
        return false;
    }

    auto get_string = [](const nlohmann::json& obj, const char* key, std::string& out) {
        auto it = obj.find(key);
        if (it == obj.end() || !it->is_string())
            return false;
        out = it->get<std::string>();
        return !out.empty();
    };
    auto get_font = [&](const char* key, LocFontFiles& out) {
        auto it = j.find(key);
        if (it == j.end())
            return;
        out.declared = true;
        if (it->is_object()) {
            get_string(*it, "image", out.image);
            get_string(*it, "map", out.map);
        }
    };

    std::string display, table, codepage;
    if (!get_string(j, "display_name", display)) {
        reason = LOC_MANIFEST_FILE " has no \"display_name\"";
        return false;
    }
    if (!get_string(j, "table", table)) {
        reason = LOC_MANIFEST_FILE " has no \"table\"";
        return false;
    }
    if (!get_string(j, "codepage", codepage)) {
        reason = LOC_MANIFEST_FILE " has no \"codepage\"";
        return false;
    }
    const char* why;
    if (!LOC_check_name(table, &why)) {
        reason = std::string("bad \"table\": ") + why;
        return false;
    }
    info.codepage = LOC_find_codepage(codepage);
    if (!info.codepage) {
        reason = "unknown codepage '" + codepage + "'";
        return false;
    }
    if (!fs::is_regular_file(dir / LOC_path_from_utf8(table), ec)) {
        reason = "table file '" + table + "' is missing";
        return false;
    }
    info.dir = dir;
    info.table_file = table;
    info.display_name = LOC_utf8_to_codepage_lossy(display, *info.codepage);
    get_font("font2d", info.font2d);
    get_font("menufont", info.menufont);
    auto voice = j.find("voice");
    info.voice = voice != j.end() && voice->is_boolean() && voice->get<bool>();
    return true;
}

static bool known_mod(std::string_view name)
{
    for (const LocModInfo& m : s_mods) {
        if (LOC_same_name(m.name, name))
            return true;
    }
    return false;
}

static void scan_root(const fs::path& root, bool user_folder)
{
    std::error_code ec;
    if (!fs::is_directory(root, ec))
        return;

    std::vector<fs::path> dirs;
    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code ec2;
        if (it->is_directory(ec2))
            dirs.push_back(it->path());
    }
    std::sort(dirs.begin(), dirs.end());

    for (const fs::path& dir : dirs) {
        LocModInfo info;
        info.name = LOC_path_to_utf8(dir.filename());
        info.user_folder = user_folder;
        // The user folder wins, even when its copy is broken.
        if (known_mod(info.name) || LOC_discovery_skipped(info.name))
            continue;

        const char* why;
        std::string reason;
        if (!LOC_check_name(info.name, &why))
            reason = why;
        else
            read_manifest(dir, info, reason); // sets reason on failure
        if (!reason.empty()) {
            fprintf(stderr, LOC_LOG_PREFIX "skipped '%s': %s\n", LOC_path_to_utf8(dir).c_str(), reason.c_str());
            s_skipped.push_back(info.name);
            continue;
        }
        s_mods.push_back(info);
    }
}

const std::vector<LocModInfo>& LOC_discover(void)
{
    if (s_discovered)
        return s_mods;
    s_discovered = true;

    const char* user_root = USERDATA_root();
    if (user_root[0])
        scan_root(LOC_path_from_utf8(user_root) / LOC_MODS_DIR, true);
    // The install folder is the working directory (the file layer's base path
    // is always empty, see FileSetBasePath at startup).
    scan_root(fs::path(LOC_MODS_DIR), false);

    std::sort(s_mods.begin(), s_mods.end(), [](const LocModInfo& a, const LocModInfo& b) { return a.name < b.name; });
    return s_mods;
}

bool LOC_discovery_skipped(std::string_view name)
{
    for (const std::string& s : s_skipped) {
        if (LOC_same_name(s, name))
            return true;
    }
    return false;
}
