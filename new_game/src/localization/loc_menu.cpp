// Localization mods: the Language item of the Options menu.
//
// The item is an ordinary multi-choice row, so the menu's own code cycles it and
// draws it: English first, then every discovered mod. The choice is saved when
// the screen is left and applies after a restart. Nothing is switched live: that
// would mean rebuilding the string pool, the font atlases and every pointer into
// the pool from inside the menu.

#include "engine/platform/uc_common.h"
#include "localization/localization.h"
#include "localization/loc_internal.h"

#include "feature_flags.h"
#include "engine/graphics/text/menufont.h" // MENUFONT_Dimensions
#include "engine/io/oc_config.h" // OC_CONFIG_get_string, OC_CONFIG_set_string
#include "ui/frontend/frontend.h" // OT_*, BIG_FONT_SCALE
#include "ui/frontend/frontend_globals.h" // menu_data, menu_state

#include <algorithm>
#include <cstring>

// Texts of the item. Looked up in the table like every other own string. The
// English entry is not: a language is listed under its own name, and this one
// must stay readable with any mod.
static const char LOC_MENU_LABEL[] = "Language";
static const char LOC_MENU_RESTART_NOTE[] = "Restart to apply";
static const char LOC_MENU_ENGLISH[] = "English";

// Layout of the rows, as FRONTEND_easy computes it for items from raw_menu_data:
// multi-choice labels start at x = 30, label rows are centred on the 640 px screen.
// uc_orig: FRONTEND_easy (fallen/Source/frontend.cpp)
#define LOC_MENU_MULTI_X 30
#define LOC_MENU_CENTRE_X 320

// MenuData::Data of a multi-choice row: the current choice in the low byte, the
// number of choices above it.
// uc_orig: FRONTEND_easy (fallen/Source/frontend.cpp)
#define LOC_MENU_DATA_INDEX_MASK 0xff
#define LOC_MENU_DATA_COUNT_SHIFT 8
// The menu steps to the next choice by adding one to the low byte before it
// compares with the count, so the count (English + mods) has to fit that byte.
#define LOC_MENU_MAX_MODS (LOC_MENU_DATA_INDEX_MASK - 1)

#define LOC_MENU_DATA_ROWS ((int)(sizeof(menu_data) / sizeof(menu_data[0])))

static std::string s_choices; // the choices, each with its terminator, as the row expects
static int s_row = -1; // the Language row in menu_data, -1 when the screen has none
static int s_shown_choice = 0; // what the config holds, to save only a real change

// What to show for a mod. Its display_name needs the font of its own language, and
// only the running mod's font is loaded, so a name it cannot draw gives way to the
// folder name, which the player can at least read and match.
static std::string choice_label(int mod)
{
    const std::string_view display = LOC_mod_display_name(mod);
    if (LOC_font_covers(display, true))
        return std::string(display);
    std::string folder = LOC_mod_name(mod);
    for (char& c : folder) {
        if ((unsigned char)c >= LOC_ASCII_LIMIT)
            c = '?';
    }
    return folder;
}

// The choice matching the config: 0 is English, a stale name also falls back to it.
static int stored_choice(int mods)
{
    const std::string value = OC_CONFIG_get_string(LOC_CONFIG_SECTION, LOC_CONFIG_KEY, "");
    const std::string_view name = LOC_trim(value);
    if (name.empty())
        return 0;
    for (int i = 0; i < mods; i++) {
        if (LOC_same_name(LOC_mod_name(i), name))
            return i + 1;
    }
    return 0;
}

static void fill_label_row(MenuData& row, const char* english)
{
    SLONG w, h;
    row.Type = OT_LABEL;
    row.Label = LOC_own_menu(english);
    MENUFONT_Dimensions(row.Label, w, h, -1, BIG_FONT_SCALE);
    row.X = (UWORD)(LOC_MENU_CENTRE_X - (w >> 1));
}

void LOC_language_menu_add(void)
{
    s_row = -1;
    if (!g_feature_flags.ext_localization_mods)
        return;

    const int mods = std::min(LOC_mod_count(), LOC_MENU_MAX_MODS);
    // With no mod to choose there is nothing to restart for.
    const int added = mods ? 2 : 1;
    if (!menu_state.items || menu_state.items + added > LOC_MENU_DATA_ROWS)
        return;

    s_choices.assign(LOC_MENU_ENGLISH, sizeof(LOC_MENU_ENGLISH));
    for (int i = 0; i < mods; i++) {
        s_choices += choice_label(i);
        s_choices += '\0';
    }
    s_shown_choice = stored_choice(mods);

    // Our rows go before the last one, which is the screen's Okay button.
    s_row = menu_state.items - 1;
    menu_data[s_row + added] = menu_data[s_row];
    memset(&menu_data[s_row], 0, added * sizeof(MenuData));
    menu_state.items += added;

    MenuData& row = menu_data[s_row];
    row.Type = OT_MULTI;
    row.Label = LOC_own_menu(LOC_MENU_LABEL);
    row.Choices = s_choices.data();
    row.Data = s_shown_choice | ((mods + 1) << LOC_MENU_DATA_COUNT_SHIFT);
    row.X = LOC_MENU_MULTI_X;

    if (mods)
        fill_label_row(menu_data[s_row + 1], LOC_MENU_RESTART_NOTE);
}

void LOC_language_menu_store(void)
{
    if (s_row < 0)
        return;
    const int choice = menu_data[s_row].Data & LOC_MENU_DATA_INDEX_MASK;
    if (choice == s_shown_choice)
        return; // a stale name in the config stays until the player picks something
    s_shown_choice = choice;
    OC_CONFIG_set_string(LOC_CONFIG_SECTION, LOC_CONFIG_KEY, choice ? LOC_mod_name(choice - 1) : "");
}
