// Localization mods: the optional font atlases for bytes >= 128.
//
// Fonts are per-byte. A byte the mod's map defines draws from the mod atlas;
// every other byte keeps the game's lookup and art, so mixed Latin and non-Latin
// text works and a mod without a font changes nothing.
//
// font2d.tga: uncompressed 32-bit TGA, alpha = coverage, RGB white. Tinted by
// the vertex colour, alpha blended like the game's FONT2D page.
// font2d.map: one glyph per row, values separated by tabs or spaces, '#' begins
// a comment line:
//     byte  x  y  w  advance
// x, y = top-left of the glyph cell in atlas pixels, w = ink width, advance = pen
// advance, in pixels at scale 256. The cell is always LOC_FONT2D_CELL_H rows tall,
// the height FONT2D draws; the baseline is row LOC_FONT2D_BASELINE_ROW of the cell.
// Bytes 0x80..0xFF only.
//
// menufont.tga: uncompressed 32-bit TGA. RGB = intensity (the page is additive),
// alpha = coverage for the title's shadow pass.
// menufont.map: same layout with the fifth column h instead of advance:
//     byte  x  y  w  h
// w, h = on-screen size at scale 256; the pen advance is w - 1, as for the game's
// menu font. Bytes 0x80..0xFF and lowercase a..z.
//
// A map row with an unknown byte, an empty rectangle or a rectangle outside the
// image rejects that whole font.

#include "engine/platform/uc_common.h"
#include "localization/localization.h"
#include "localization/loc_internal.h"

#include "assets/texture.h" // TEXTURE_page_*_l10n
#include "engine/graphics/graphics_engine/game_graphics_engine.h" // ge_texture_load_rgba
#include "engine/graphics/pipeline/poly.h" // POLY_PAGE_*
#include "engine/graphics/pipeline/poly_globals.h" // POLY_Page
#include "engine/graphics/pipeline/polypage.h" // UI affine for pixel-snapping letters
#include "engine/graphics/text/menufont.h" // FontInfo

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

// uc_orig: PANEL_draw_quad (fallen/DDEngine/Source/panel.cpp)
extern void PANEL_draw_quad(float left, float top, float right, float bottom,
    SLONG page, ULONG colour, float u1, float v1, float u2, float v2);

// One slot per byte value.
#define LOC_BYTE_COUNT 256
// First byte a mod font may redefine: below it the game's art is kept.
#define LOC_FIRST_FONT_BYTE 0x80

// FONT2D_DrawLetter's quad: y - 3 .. y + (15 * scale >> 8), with a V span of
// 18 texels. Those numbers are hardcoded in the game's draw path, and the game's
// atlas puts its baseline 14 rows below the top of that span.
// uc_orig: FONT2D_DrawLetter (fallen/DDEngine/Source/font2d.cpp)
#define LOC_FONT2D_QUAD_TOP 3
#define LOC_FONT2D_QUAD_BOTTOM 15
#define LOC_FONT2D_CELL_H 18
#define LOC_FONT2D_BASELINE_ROW 14

// CharData stores the glyph size in a UBYTE, and no text glyph comes near that.
#define LOC_GLYPH_MAX_SIZE 255

// The atlas is kept in RAM to be uploaded again after every level load. 1024
// is well above any sensible glyph atlas and keeps that at 4 MB per font.
#define LOC_ATLAS_MAX_SIZE 1024

// Uncompressed true-colour TGA: 18-byte header, optional id field, then pixels.
// Header offsets are from the TGA specification.
#define LOC_TGA_HEADER_SIZE 18
#define LOC_TGA_ID_LENGTH 0
#define LOC_TGA_COLOR_MAP_TYPE 1
#define LOC_TGA_IMAGE_TYPE 2
#define LOC_TGA_WIDTH 12
#define LOC_TGA_HEIGHT 14
#define LOC_TGA_PIXEL_DEPTH 16
#define LOC_TGA_DESCRIPTOR 17
#define LOC_TGA_TYPE_TRUE_COLOR 2
#define LOC_TGA_DEPTH_32 32
#define LOC_TGA_BYTES_PER_PIXEL 4
// Descriptor bit: rows are stored top-down (the usual order is bottom-up).
#define LOC_TGA_TOP_ORIGIN 0x20
// The id field's length is one byte.
#define LOC_TGA_ID_MAX 255
#define LOC_TGA_MAX_FILE_BYTES \
    (LOC_TGA_HEADER_SIZE + LOC_TGA_ID_MAX + LOC_ATLAS_MAX_SIZE * LOC_ATLAS_MAX_SIZE * LOC_TGA_BYTES_PER_PIXEL)

// A map has at most one row per byte value plus comments.
#define LOC_MAP_MAX_BYTES (64 * 1024)
// Columns of a map row: byte, x, y, w, and advance or h.
#define LOC_MAP_COLUMNS 5
// Map numbers are decimal, or hex with a 0x prefix (so a leading 0 is not octal).
#define LOC_MAP_BASE_DEC 10
#define LOC_MAP_BASE_HEX 16

struct LocGlyph {
    float u1, v1, u2, v2;
    SLONG w; // ink width (FONT2D) or on-screen width (menu)
    SLONG size2; // pen advance (FONT2D) or on-screen height (menu)
};

struct LocFont {
    bool loaded = false;
    bool mapped[LOC_BYTE_COUNT] = {};
    LocGlyph glyph[LOC_BYTE_COUNT] = {};
    SLONG width = 0;
    SLONG height = 0;
    std::vector<uint8_t> rgba; // top-left origin, R,G,B,A in memory
};

static LocFont s_font2d;
static LocFont s_menufont;

// The game's menu glyph table as MENUFONT_MergeLower left it, before any mod glyph.
static CharData s_menu_game_info[LOC_BYTE_COUNT];
static bool s_menu_game_saved = false;

// Reads the atlas image into font.rgba.
static bool load_atlas(const std::filesystem::path& file, LocFont& font, std::string& why)
{
    std::string data;
    if (!LOC_read_file(file, LOC_TGA_MAX_FILE_BYTES, data)) {
        why = "cannot read the image, or it is too large";
        return false;
    }
    if (data.size() < LOC_TGA_HEADER_SIZE) {
        why = "the image is not a TGA";
        return false;
    }
    const uint8_t* h = (const uint8_t*)data.data();
    if (h[LOC_TGA_COLOR_MAP_TYPE] != 0 || h[LOC_TGA_IMAGE_TYPE] != LOC_TGA_TYPE_TRUE_COLOR
        || h[LOC_TGA_PIXEL_DEPTH] != LOC_TGA_DEPTH_32) {
        why = "the image is not an uncompressed 32-bit TGA";
        return false;
    }
    const SLONG w = h[LOC_TGA_WIDTH] | (h[LOC_TGA_WIDTH + 1] << 8);
    const SLONG height = h[LOC_TGA_HEIGHT] | (h[LOC_TGA_HEIGHT + 1] << 8);
    if (w <= 0 || height <= 0 || w > LOC_ATLAS_MAX_SIZE || height > LOC_ATLAS_MAX_SIZE) {
        why = "bad image size";
        return false;
    }
    const size_t pixels_at = (size_t)LOC_TGA_HEADER_SIZE + h[LOC_TGA_ID_LENGTH];
    const size_t row_bytes = (size_t)w * LOC_TGA_BYTES_PER_PIXEL;
    if (data.size() < pixels_at + row_bytes * height) {
        why = "the image file is truncated";
        return false;
    }

    const bool top_down = (h[LOC_TGA_DESCRIPTOR] & LOC_TGA_TOP_ORIGIN) != 0;
    font.rgba.resize(row_bytes * height);
    for (SLONG y = 0; y < height; y++) {
        const uint8_t* src = h + pixels_at + row_bytes * (top_down ? y : height - 1 - y);
        uint8_t* dst = font.rgba.data() + row_bytes * y;
        for (SLONG x = 0; x < w; x++, src += LOC_TGA_BYTES_PER_PIXEL, dst += LOC_TGA_BYTES_PER_PIXEL) {
            dst[0] = src[2]; // TGA pixels are B,G,R,A
            dst[1] = src[1];
            dst[2] = src[0];
            dst[3] = src[3];
        }
    }
    font.width = w;
    font.height = height;
    return true;
}

// Splits a map row into its numeric columns. False if the row is malformed.
static bool parse_map_row(std::string_view line, long col[LOC_MAP_COLUMNS])
{
    int n = 0;
    size_t i = 0;
    while (i < line.size()) {
        if (line[i] == ' ' || line[i] == '\t') {
            i++;
            continue;
        }
        size_t end = i;
        while (end < line.size() && line[end] != ' ' && line[end] != '\t')
            end++;
        if (n == LOC_MAP_COLUMNS)
            return false;
        const std::string token(line.substr(i, end - i));
        char* stop;
        const int base = (token.rfind("0x", 0) == 0 || token.rfind("0X", 0) == 0) ? LOC_MAP_BASE_HEX : LOC_MAP_BASE_DEC;
        col[n++] = strtol(token.c_str(), &stop, base);
        if (*stop)
            return false;
        i = end;
    }
    return n == LOC_MAP_COLUMNS;
}

// Reads the glyph map. The image must already be loaded: rectangles are checked
// against it.
static bool load_map(const std::filesystem::path& file, LocFont& font, bool menu, std::string& why)
{
    std::string text;
    if (!LOC_read_file(file, LOC_MAP_MAX_BYTES, text)) {
        why = "cannot read the map";
        return false;
    }

    int glyphs = 0;
    int row = 0;
    size_t pos = 0;
    if (text.compare(0, sizeof(LOC_UTF8_BOM) - 1, LOC_UTF8_BOM) == 0)
        pos = sizeof(LOC_UTF8_BOM) - 1;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos)
            eol = text.size();
        std::string_view line(text.data() + pos, eol - pos);
        pos = eol + 1;
        row++;

        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        line = LOC_trim(line);
        if (line.empty() || line[0] == '#')
            continue;

        const std::string at = "map row " + std::to_string(row) + ": ";
        long col[LOC_MAP_COLUMNS];
        if (!parse_map_row(line, col)) {
            why = at + "expected five numbers";
            return false;
        }
        const long byte = col[0];
        const long x = col[1];
        const long y = col[2];
        const long w = col[3];
        const long size2 = col[4];
        const bool font_byte = byte >= LOC_FIRST_FONT_BYTE && byte < LOC_BYTE_COUNT;
        const bool menu_lower = menu && byte >= 'a' && byte <= 'z';
        if (!font_byte && !menu_lower) {
            why = at + "this byte cannot be redefined";
            return false;
        }
        if (font.mapped[byte]) {
            why = at + "this byte is defined twice";
            return false;
        }
        if (w < 1 || size2 < 1 || w > LOC_GLYPH_MAX_SIZE || size2 > LOC_GLYPH_MAX_SIZE) {
            why = at + "empty or oversized glyph";
            return false;
        }
        const long cell_h = menu ? size2 : LOC_FONT2D_CELL_H;
        if (x < 0 || y < 0 || x + w > font.width || y + cell_h > font.height) {
            why = at + "the rectangle is outside the image";
            return false;
        }

        LocGlyph& g = font.glyph[byte];
        g.u1 = float(x) / float(font.width);
        g.v1 = float(y) / float(font.height);
        g.u2 = float(x + w) / float(font.width);
        g.v2 = float(y + cell_h) / float(font.height);
        g.w = w;
        g.size2 = size2;
        font.mapped[byte] = true;
        glyphs++;
    }
    if (!glyphs) {
        why = "the map has no glyphs";
        return false;
    }
    return true;
}

static bool load_font(const std::filesystem::path& dir, const LocFontFiles& files, LocFont& font, bool menu,
    std::string& why)
{
    const char* bad;
    if (files.image.empty() || files.map.empty()) {
        why = "the manifest entry needs \"image\" and \"map\"";
        return false;
    }
    if (!LOC_check_name(files.image, &bad) || !LOC_check_name(files.map, &bad)) {
        why = std::string("bad file name: ") + bad;
        return false;
    }
    return load_atlas(dir / LOC_path_from_utf8(files.image), font, why)
        && load_map(dir / LOC_path_from_utf8(files.map), font, menu, why);
}

void LOC_load_fonts(const LocModInfo& mod)
{
    struct {
        const char* name;
        const LocFontFiles& files;
        LocFont& font;
        bool menu;
    } const fonts[] = {
        { "font2d", mod.font2d, s_font2d, false },
        { "menufont", mod.menufont, s_menufont, true },
    };
    for (const auto& f : fonts) {
        if (!f.files.declared)
            continue;
        std::string why;
        if (load_font(mod.dir, f.files, f.font, f.menu, why)) {
            f.font.loaded = true;
        } else {
            f.font = LocFont {};
            fprintf(stderr, LOC_LOG_PREFIX "%s: %s rejected (%s), using the game's font\n", mod.name.c_str(), f.name,
                why.c_str());
        }
    }
}

bool LOC_font_covers(std::string_view text, bool menu)
{
    const LocFont& font = menu ? s_menufont : s_font2d;
    for (char c : text) {
        const UBYTE b = (UBYTE)c;
        if (b >= LOC_FIRST_FONT_BYTE && !font.mapped[b])
            return false;
    }
    return true;
}

bool LOC_font2d_mapped(CBYTE chr)
{
    return s_font2d.mapped[(UBYTE)chr];
}

SLONG LOC_font2d_advance(CBYTE chr)
{
    return s_font2d.glyph[(UBYTE)chr].size2;
}

// The same quad as the game's letters, pixel-snapped the same way: without the
// snap, mod glyphs would sit on a different pixel grid than the letters around them.
// uc_orig: FONT2D_DrawLetter (fallen/DDEngine/Source/font2d.cpp)
SLONG LOC_font2d_draw(CBYTE chr, SLONG x, SLONG y, ULONG rgb, SLONG scale, SWORD fade)
{
    const LocGlyph& g = s_font2d.glyph[(UBYTE)chr];

    SATURATE(fade, 0, 255);

    float sx = PolyPage::ui_scale_x();
    float sy = PolyPage::ui_scale_y();
    const float ox = PolyPage::ui_offset_x();
    const float oy = PolyPage::ui_offset_y();
    if (sx <= 0.0f)
        sx = 1.0f;
    if (sy <= 0.0f)
        sy = 1.0f;
    auto snap_x = [&](float vx) { return ((float)(SLONG)(vx * sx + ox + 0.5f) - ox) / sx; };
    auto snap_y = [&](float vy) { return ((float)(SLONG)(vy * sy + oy + 0.5f) - oy) / sy; };

    PANEL_draw_quad(
        snap_x((float)x),
        snap_y((float)(y - LOC_FONT2D_QUAD_TOP)),
        snap_x((float)(x + (g.w * scale >> 8))),
        snap_y((float)(y + (LOC_FONT2D_QUAD_BOTTOM * scale >> 8))),
        POLY_PAGE_FONT2D_L10N,
        (rgb & 0xffffff) | ((255 - fade) << 24),
        g.u1,
        g.v1,
        g.u2,
        g.v2);

    return g.size2 * scale >> 8;
}

// Writes a mapped byte's glyph into the live menu glyph table.
static void install_menu_glyph(int b)
{
    const LocGlyph& g = s_menufont.glyph[b];
    CharData& cd = FontInfo[b];
    cd.x = g.u1;
    cd.y = g.v1;
    cd.ox = g.u2;
    cd.oy = g.v2;
    cd.width = (UBYTE)g.w;
    cd.height = (UBYTE)g.size2;
    cd.xofs = 0;
    cd.yofs = 0;
}

void LOC_menu_font_install(void)
{
    if (!s_menufont.loaded)
        return;

    // MENUFONT_Load reads the font only once, so from the second FRONTEND_init on
    // MENUFONT_MergeLower runs over our glyphs and copies a mapped uppercase byte
    // into its lowercase slot. An unmapped slot would then hold mod UVs but draw
    // from the game's page. Start every install from the game's own table.
    if (s_menu_game_saved) {
        memcpy(FontInfo, s_menu_game_info, sizeof(s_menu_game_info));
    } else {
        memcpy(s_menu_game_info, FontInfo, sizeof(s_menu_game_info));
        s_menu_game_saved = true;
    }

    for (int b = 0; b < LOC_BYTE_COUNT; b++) {
        if (s_menufont.mapped[b])
            install_menu_glyph(b);
    }
}

void LOC_menu_font_alt_install(void)
{
    if (!s_menufont.loaded)
        return;
    // Only bytes the alt font lacks: its ASCII glyphs stay, so an untranslated
    // string looks exactly as without a mod.
    for (int b = LOC_FIRST_FONT_BYTE; b < LOC_BYTE_COUNT; b++) {
        if (s_menufont.mapped[b])
            install_menu_glyph(b);
    }
}

SLONG LOC_menu_page(UBYTE ch, SLONG page)
{
    if (!s_menufont.mapped[ch])
        return page;
    // The game's menu font draws through these two pages, one of them only for
    // title shadows.
    if (page == POLY_PAGE_NEWFONT_INVERSE)
        return POLY_PAGE_MENUFONT_L10N;
    if (page == POLY_PAGE_NEWFONT)
        return POLY_PAGE_MENUFONT_L10N_SHADOW;
    // Inside MENUFONT_AltScope FontInfo holds the alt font plus the mod's bytes
    // >= 128 (LOC_menu_font_alt_install); a mapped lowercase byte is still alt.
    if (page == POLY_PAGE_MENUFONT_ALT && ch >= LOC_FIRST_FONT_BYTE)
        return POLY_PAGE_MENUFONT_L10N;
    return page;
}

void LOC_font_on_textures_ready(void)
{
    if (s_font2d.loaded)
        ge_texture_load_rgba(TEXTURE_page_font2d_l10n, s_font2d.width, s_font2d.height, s_font2d.rgba.data());
    if (s_menufont.loaded)
        ge_texture_load_rgba(TEXTURE_page_menufont_l10n, s_menufont.width, s_menufont.height, s_menufont.rgba.data());
}

// --- Draw order of the mod font pages ---
//
// The mod pages sit below every other special page (POLY_NUM_PAGES - 113..115),
// so the page loops of POLY_frame_draw would draw them before the HUD's
// translucent pages. A speech bubble (POLY_PAGE_LASTPANEL_ALPHA, - 104) would
// then cover the mod letters and grey them, while the game's FONT2D letters
// (- 39) draw after the bubble and stay white. Each mod page therefore takes
// the draw slot right after the game page it mirrors. Without a mod font the
// mod pages are empty, so the moved slots draw nothing.

// The game page whose draw slot a mod page takes, or -1 for any other page.
static SLONG l10n_mirror(SLONG page)
{
    switch (page) {
    case POLY_PAGE_FONT2D_L10N:
        return POLY_PAGE_FONT2D;
    case POLY_PAGE_MENUFONT_L10N:
        return POLY_PAGE_NEWFONT_INVERSE;
    case POLY_PAGE_MENUFONT_L10N_SHADOW:
        return POLY_PAGE_NEWFONT;
    }
    return -1;
}

static const SLONG l10n_pages[] = {
    POLY_PAGE_FONT2D_L10N,
    POLY_PAGE_MENUFONT_L10N,
    POLY_PAGE_MENUFONT_L10N_SHADOW,
};

SLONG LOC_page_in_draw_order(SLONG pos)
{
    static SLONG order[POLY_NUM_PAGES];
    static bool built = false;
    if (!built) {
        SLONG n = 0;
        for (SLONG page = 0; page < POLY_NUM_PAGES; page++) {
            if (l10n_mirror(page) >= 0)
                continue;
            order[n++] = page;
            for (SLONG l : l10n_pages) {
                if (l10n_mirror(l) == page)
                    order[n++] = l;
            }
        }
        built = true;
    }
    return order[pos];
}

static SLONG find_page(const SLONG* order, SLONG count, SLONG page)
{
    for (SLONG i = 0; i < count; i++) {
        if (order[i] == page)
            return i;
    }
    return -1;
}

void LOC_page_order_fix(SLONG* order, SLONG count)
{
    for (SLONG l : l10n_pages) {
        const SLONG from = find_page(order, count, l);
        if (from < 0)
            continue; // a ghost page: its polys go to another page
        memmove(&order[from], &order[from + 1], (count - from - 1) * sizeof(SLONG));
        // The mirrored page may itself be a ghost; its polys draw with its real page.
        const SLONG mirror = (SLONG)(POLY_Page[l10n_mirror(l)].pTheRealPolyPage - POLY_Page);
        SLONG to = find_page(order, count - 1, mirror);
        to = (to < 0) ? from : to + 1;
        memmove(&order[to + 1], &order[to], (count - 1 - to) * sizeof(SLONG));
        order[to] = l;
    }
}
