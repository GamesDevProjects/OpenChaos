// Localization mods: the mapping table (UTF-8 TSV) and codepage conversion.
//
// Table format, one row per line (LF or CRLF), '#' at the start of a line
// begins a comment, blank lines are ignored:
//
//     key <TAB> value
//     key <TAB> value <TAB> context
//
// key   = the English text exactly as the engine has it in memory.
// value = the translation. An empty value means "not translated yet".
// context (optional) = lang | ucm | txt | sty | own; ucm and txt both name the
//         in-mission message surface (one load path serves both), own names
//         the strings OpenChaos added to the game.
// Escapes in key and value: \\ \t \r \n and \xNN (one raw byte, NN >= 0x20).
// "|N" is an engine control token, not an escape: a value must carry exactly
// the same |N tokens (and bare '|' conversation separators) as its key.
//
// Errors that make the file unreadable (bad escape, invalid UTF-8, missing
// TAB, unknown context, control character) reject the whole mod. Errors that
// concern one row (token mismatch, a character the codepage lacks) drop that
// row with a warning; the English text is then used for it.

#include "localization/loc_internal.h"

#include <algorithm>
#include <cstdio>
#include <fstream>

// The table is read whole into memory; a complete translation of every game
// text is well under 1 MB, so this only guards against pointing the loader at
// a wrong, huge file.
#define LOC_TABLE_MAX_BYTES (16 * 1024 * 1024)

// Row-level warnings printed per table; the rest are counted in one line.
#define LOC_MAX_ROW_WARNINGS 20

// Bytes below this are control characters. They are refused in table text:
// byte 0x01 in particular is the lang pool's control-token marker.
#define LOC_FIRST_PRINTABLE 0x20

// Columns of a row: key, value, optional context.
#define LOC_TABLE_MIN_COLUMNS 2
#define LOC_TABLE_MAX_COLUMNS 3

// Windows-1251 (Cyrillic): Unicode code point of bytes 0x80..0xFF.
static const uint16_t LOC_CP1251_HIGH[LOC_CODEPAGE_HIGH_COUNT] = {
    0x0402, 0x0403, 0x201A, 0x0453, 0x201E, 0x2026, 0x2020, 0x2021, // 0x80
    0x20AC, 0x2030, 0x0409, 0x2039, 0x040A, 0x040C, 0x040B, 0x040F, // 0x88
    0x0452, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, // 0x90
    0x0000, 0x2122, 0x0459, 0x203A, 0x045A, 0x045C, 0x045B, 0x045F, // 0x98
    0x00A0, 0x040E, 0x045E, 0x0408, 0x00A4, 0x0490, 0x00A6, 0x00A7, // 0xA0
    0x0401, 0x00A9, 0x0404, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x0407, // 0xA8
    0x00B0, 0x00B1, 0x0406, 0x0456, 0x0491, 0x00B5, 0x00B6, 0x00B7, // 0xB0
    0x0451, 0x2116, 0x0454, 0x00BB, 0x0458, 0x0405, 0x0455, 0x0457, // 0xB8
    0x0410, 0x0411, 0x0412, 0x0413, 0x0414, 0x0415, 0x0416, 0x0417, // 0xC0
    0x0418, 0x0419, 0x041A, 0x041B, 0x041C, 0x041D, 0x041E, 0x041F, // 0xC8
    0x0420, 0x0421, 0x0422, 0x0423, 0x0424, 0x0425, 0x0426, 0x0427, // 0xD0
    0x0428, 0x0429, 0x042A, 0x042B, 0x042C, 0x042D, 0x042E, 0x042F, // 0xD8
    0x0430, 0x0431, 0x0432, 0x0433, 0x0434, 0x0435, 0x0436, 0x0437, // 0xE0
    0x0438, 0x0439, 0x043A, 0x043B, 0x043C, 0x043D, 0x043E, 0x043F, // 0xE8
    0x0440, 0x0441, 0x0442, 0x0443, 0x0444, 0x0445, 0x0446, 0x0447, // 0xF0
    0x0448, 0x0449, 0x044A, 0x044B, 0x044C, 0x044D, 0x044E, 0x044F, // 0xF8
};

static const LocCodepage LOC_CODEPAGES[] = {
    { "CP1251", "windows-1251", LOC_CP1251_HIGH },
};

// Largest valid Unicode code point and the UTF-16 surrogate range (invalid in UTF-8).
#define LOC_UNICODE_MAX 0x10FFFF
#define LOC_SURROGATE_FIRST 0xD800
#define LOC_SURROGATE_LAST 0xDFFF

static bool ascii_iequal(std::string_view a, std::string_view b)
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

const LocCodepage* LOC_find_codepage(std::string_view name)
{
    for (const LocCodepage& cp : LOC_CODEPAGES) {
        if (ascii_iequal(name, cp.name) || ascii_iequal(name, cp.alias))
            return &cp;
    }
    return nullptr;
}

// Byte for a code point >= 0x80, or -1 if the codepage has none.
static int codepage_byte(const LocCodepage& cp, uint32_t code)
{
    for (int i = 0; i < LOC_CODEPAGE_HIGH_COUNT; i++) {
        if (cp.high[i] != 0 && cp.high[i] == code)
            return LOC_ASCII_LIMIT + i;
    }
    return -1;
}

// Decodes one UTF-8 sequence at s[i]. Returns false on malformed input.
static bool utf8_next(std::string_view s, size_t& i, uint32_t& code)
{
    const unsigned char c = (unsigned char)s[i];
    int extra;
    uint32_t min_code;
    if (c < 0x80) {
        code = c;
        i++;
        return true;
    } else if ((c & 0xE0) == 0xC0) {
        extra = 1;
        code = c & 0x1F;
        min_code = 0x80;
    } else if ((c & 0xF0) == 0xE0) {
        extra = 2;
        code = c & 0x0F;
        min_code = 0x800;
    } else if ((c & 0xF8) == 0xF0) {
        extra = 3;
        code = c & 0x07;
        min_code = 0x10000;
    } else {
        return false;
    }
    if (i + extra >= s.size())
        return false;
    for (int k = 1; k <= extra; k++) {
        const unsigned char cc = (unsigned char)s[i + k];
        if ((cc & 0xC0) != 0x80)
            return false;
        code = (code << 6) | (cc & 0x3F);
    }
    if (code < min_code || code > LOC_UNICODE_MAX || (code >= LOC_SURROGATE_FIRST && code <= LOC_SURROGATE_LAST))
        return false;
    i += extra + 1;
    return true;
}

std::string LOC_utf8_to_codepage_lossy(std::string_view utf8, const LocCodepage& cp)
{
    std::string out;
    size_t i = 0;
    while (i < utf8.size()) {
        uint32_t code;
        if (!utf8_next(utf8, i, code)) {
            out += '?';
            i++;
            continue;
        }
        if (code < LOC_FIRST_PRINTABLE) {
            out += '?';
        } else if (code < LOC_ASCII_LIMIT) {
            out += (char)code;
        } else {
            const int b = codepage_byte(cp, code);
            out += (b < 0) ? '?' : (char)b;
        }
    }
    return out;
}

bool LOC_read_file(const std::filesystem::path& file, size_t max_bytes, std::string& out)
{
    std::error_code ec;
    const uintmax_t size = std::filesystem::file_size(file, ec);
    if (ec || size > max_bytes)
        return false;
    std::ifstream f(file, std::ios::binary);
    if (!f)
        return false;
    out.resize((size_t)size);
    if (size && !f.read(&out[0], (std::streamsize)size))
        return false;
    return true;
}

std::string_view LOC_trim(std::string_view s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
        s.remove_suffix(1);
    return s;
}

// Result of decoding one table field.
enum LocFieldResult {
    LOC_FIELD_OK,
    LOC_FIELD_SYNTAX, // the file is malformed: the mod is rejected
    LOC_FIELD_UNMAPPABLE, // a character the codepage lacks: the row is dropped
};

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

// Decodes escapes and UTF-8 of one field into codepage bytes. On failure
// writes the reason into `why`.
static LocFieldResult decode_field(std::string_view in, const LocCodepage& cp, std::string& out, std::string& why)
{
    out.clear();
    size_t i = 0;
    while (i < in.size()) {
        const char c = in[i];
        if (c == '\\') {
            if (i + 1 >= in.size()) {
                why = "backslash at the end of a field";
                return LOC_FIELD_SYNTAX;
            }
            const char e = in[i + 1];
            switch (e) {
            case '\\':
                out += '\\';
                i += 2;
                continue;
            case 't':
                out += '\t';
                i += 2;
                continue;
            case 'r':
                out += '\r';
                i += 2;
                continue;
            case 'n':
                out += '\n';
                i += 2;
                continue;
            case 'x': {
                const int hi = (i + 2 < in.size()) ? hex_digit(in[i + 2]) : -1;
                const int lo = (i + 3 < in.size()) ? hex_digit(in[i + 3]) : -1;
                if (hi < 0 || lo < 0) {
                    why = "bad escape \\x (two hex digits expected)";
                    return LOC_FIELD_SYNTAX;
                }
                const int byte = hi * 16 + lo;
                if (byte < LOC_FIRST_PRINTABLE) {
                    why = "escape \\x below 0x20 (control byte)";
                    return LOC_FIELD_SYNTAX;
                }
                out += (char)byte;
                i += 4;
                continue;
            }
            default:
                why = std::string("bad escape \\") + e;
                return LOC_FIELD_SYNTAX;
            }
        }

        uint32_t code;
        if (!utf8_next(in, i, code)) {
            why = "invalid UTF-8";
            return LOC_FIELD_SYNTAX;
        }
        if (code < LOC_FIRST_PRINTABLE) {
            why = "control character (use \\t, \\r or \\n)";
            return LOC_FIELD_SYNTAX;
        }
        if (code < LOC_ASCII_LIMIT) {
            out += (char)code;
            continue;
        }
        const int b = codepage_byte(cp, code);
        if (b < 0) {
            char buf[64];
            snprintf(buf, sizeof(buf), "character U+%04X is not in %s", (unsigned)code, cp.name);
            why = buf;
            return LOC_FIELD_UNMAPPABLE;
        }
        out += (char)b;
    }
    return LOC_FIELD_OK;
}

// The |N control tokens and bare '|' separators of a string, sorted.
static std::vector<std::string> pipe_tokens(const std::string& s)
{
    std::vector<std::string> tokens;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != '|')
            continue;
        size_t j = i + 1;
        while (j < s.size() && s[j] >= '0' && s[j] <= '9')
            j++;
        tokens.push_back(s.substr(i, j - i));
        i = j - 1;
    }
    std::sort(tokens.begin(), tokens.end());
    return tokens;
}

static bool parse_context(std::string_view cell, LocSurface& out)
{
    const std::string_view s = LOC_trim(cell);
    if (s.empty()) {
        out = LOC_SURFACE_ANY;
    } else if (s == "lang") {
        out = LOC_SURFACE_LANG;
    } else if (s == "ucm" || s == "txt") {
        out = LOC_SURFACE_MESSAGE;
    } else if (s == "sty") {
        out = LOC_SURFACE_STY;
    } else if (s == "own") {
        out = LOC_SURFACE_OWN;
    } else {
        return false;
    }
    return true;
}

bool LOC_load_table(const std::filesystem::path& file, const LocCodepage& cp, LocTable& table, std::string& error)
{
    table.clear();

    std::string text;
    if (!LOC_read_file(file, LOC_TABLE_MAX_BYTES, text)) {
        error = "cannot read table '" + LOC_path_to_utf8(file.filename()) + "'";
        return false;
    }

    size_t pos = 0;
    if (text.compare(0, sizeof(LOC_UTF8_BOM) - 1, LOC_UTF8_BOM) == 0)
        pos = sizeof(LOC_UTF8_BOM) - 1;

    // Warnings are printed only if the whole file parses, so a rejected mod
    // produces exactly one log line.
    std::vector<std::string> warnings;
    int suppressed = 0;
    auto warn = [&](int row, const std::string& what) {
        if ((int)warnings.size() < LOC_MAX_ROW_WARNINGS)
            warnings.push_back("table row " + std::to_string(row) + ": " + what);
        else
            suppressed++;
    };

    std::string key, value, why;
    int row = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos)
            eol = text.size();
        std::string_view line(text.data() + pos, eol - pos);
        pos = eol + 1;
        row++;

        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        if (line.empty() || line[0] == '#')
            continue;

        std::string_view col[LOC_TABLE_MAX_COLUMNS + 1];
        int ncol = 0;
        size_t start = 0;
        while (ncol <= LOC_TABLE_MAX_COLUMNS) {
            const size_t tab = line.find('\t', start);
            col[ncol++] = line.substr(start, tab == std::string_view::npos ? std::string_view::npos : tab - start);
            if (tab == std::string_view::npos)
                break;
            start = tab + 1;
        }
        const std::string at = "table row " + std::to_string(row) + ": ";
        if (ncol < LOC_TABLE_MIN_COLUMNS) {
            error = at + "no TAB between key and value";
            return false;
        }
        if (ncol > LOC_TABLE_MAX_COLUMNS) {
            error = at + "too many columns";
            return false;
        }

        LocSurface surface = LOC_SURFACE_ANY;
        if (ncol == LOC_TABLE_MAX_COLUMNS && !parse_context(col[2], surface)) {
            error = at + "unknown context '" + std::string(col[2]) + "'";
            return false;
        }

        const LocFieldResult kr = decode_field(col[0], cp, key, why);
        if (kr == LOC_FIELD_SYNTAX) {
            error = at + why;
            return false;
        }
        if (kr == LOC_FIELD_OK && key.empty()) {
            error = at + "empty key";
            return false;
        }
        if (col[1].empty())
            continue; // not translated yet: English is used

        const LocFieldResult vr = decode_field(col[1], cp, value, why);
        if (vr == LOC_FIELD_SYNTAX) {
            error = at + why;
            return false;
        }
        if (kr == LOC_FIELD_UNMAPPABLE || vr == LOC_FIELD_UNMAPPABLE) {
            warn(row, why + ", row ignored");
            continue;
        }
        if (pipe_tokens(key) != pipe_tokens(value)) {
            warn(row, "the value does not keep the key's '|' tokens, row ignored");
            continue;
        }

        auto ins = table.map[surface].try_emplace(key);
        LocEntry& entry = ins.first->second;
        if (ins.second) {
            entry.value = value;
            entry.row = row;
            table.rows++;
        } else if (entry.value != value && !entry.dup_warned) {
            entry.dup_warned = true;
            warn(row, "key already translated differently at row " + std::to_string(entry.row) + ", the first is kept");
        }
    }

    for (const std::string& w : warnings)
        fprintf(stderr, LOC_LOG_PREFIX "%s\n", w.c_str());
    if (suppressed)
        fprintf(stderr, LOC_LOG_PREFIX "%d more table warnings not shown\n", suppressed);
    return true;
}
