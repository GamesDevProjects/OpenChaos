// Runtime feature flags — see FEATURE_FLAGS.md for descriptions.
// All OFF by default = original release behavior.
// To enable a feature, change its value to true and rebuild.

#include "feature_flags.h"

FeatureFlags g_feature_flags = {
    .cut_bats = false,
    .cut_talk_followups = true,
    .ext_in_mission_saves = true,
    .ext_localization_mods = true,
};
