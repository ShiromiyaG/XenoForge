#include "console_options.h"

#include <algorithm>

namespace xf::frontend {

const std::vector<Option> &languages() {
    // XLanguage (Xenia's xbox.h)
    static const std::vector<Option> list = {
        {0, "Default (the console's)"}, {1, "English"},  {2, "Japanese"}, {3, "German"},  {4, "French"},
        {5, "Spanish"},                 {6, "Italian"},  {7, "Korean"},   {8, "Traditional Chinese"},
        {9, "Portuguese"},              {11, "Polish"},  {12, "Russian"}, {13, "Swedish"}, {14, "Turkish"},
        {15, "Norwegian"},              {16, "Dutch"},   {17, "Simplified Chinese"},
    };
    return list;
}

const std::vector<Option> &countries() {
    // XOnlineCountry (Xenia's xbox.h)
    static const std::vector<Option> list = {
        {-1, "Default (the console's)"},
        {1, "United Arab Emirates"}, {2, "Albania"}, {3, "Armenia"}, {4, "Argentina"}, {5, "Austria"},
        {6, "Australia"}, {7, "Azerbaijan"}, {8, "Belgium"}, {9, "Bulgaria"}, {10, "Bahrain"},
        {11, "Brunei Darussalam"}, {12, "Bolivia"}, {13, "Brazil"}, {14, "Belarus"}, {15, "Belize"},
        {16, "Canada"}, {18, "Switzerland"}, {19, "Chile"}, {20, "China"}, {21, "Colombia"},
        {22, "Costa Rica"}, {23, "Czech Republic"}, {24, "Germany"}, {25, "Denmark"},
        {26, "Dominican Republic"}, {27, "Algeria"}, {28, "Ecuador"}, {29, "Estonia"}, {30, "Egypt"},
        {31, "Spain"}, {32, "Finland"}, {33, "Faroe Islands"}, {34, "France"}, {35, "Great Britain"},
        {36, "Georgia"}, {37, "Greece"}, {38, "Guatemala"}, {39, "Hong Kong"}, {40, "Honduras"},
        {41, "Croatia"}, {42, "Hungary"}, {43, "Indonesia"}, {44, "Ireland"}, {45, "Israel"},
        {46, "India"}, {47, "Iraq"}, {48, "Iran"}, {49, "Iceland"}, {50, "Italy"}, {51, "Jamaica"},
        {52, "Jordan"}, {53, "Japan"}, {54, "Kenya"}, {55, "Kyrgyzstan"}, {56, "Korea"}, {57, "Kuwait"},
        {58, "Kazakhstan"}, {59, "Lebanon"}, {60, "Liechtenstein"}, {61, "Lithuania"}, {62, "Luxembourg"},
        {63, "Latvia"}, {64, "Libya"}, {65, "Morocco"}, {66, "Monaco"}, {67, "Macedonia"},
        {68, "Mongolia"}, {69, "Macau"}, {70, "Maldives"}, {71, "Mexico"}, {72, "Malaysia"},
        {73, "Nicaragua"}, {74, "Netherlands"}, {75, "Norway"}, {76, "New Zealand"}, {77, "Oman"},
        {78, "Panama"}, {79, "Peru"}, {80, "Philippines"}, {81, "Pakistan"}, {82, "Poland"},
        {83, "Puerto Rico"}, {84, "Portugal"}, {85, "Paraguay"}, {86, "Qatar"}, {87, "Romania"},
        {88, "Russian Federation"}, {89, "Saudi Arabia"}, {90, "Sweden"}, {91, "Singapore"},
        {92, "Slovenia"}, {93, "Slovak Republic"}, {95, "El Salvador"}, {96, "Syria"}, {97, "Thailand"},
        {98, "Tunisia"}, {99, "Turkey"}, {100, "Trinidad And Tobago"}, {101, "Taiwan"}, {102, "Ukraine"},
        {103, "United States"}, {104, "Uruguay"}, {105, "Uzbekistan"}, {106, "Venezuela"},
        {107, "Viet Nam"}, {108, "Yemen"}, {109, "South Africa"}, {110, "Zimbabwe"},
    };
    return list;
}

const std::vector<Option> &resolutions() {
    static const std::vector<Option> list = {
        {0, "Default (the console's)"}, {480, "640 x 480"}, {720, "1280 x 720"}, {1080, "1920 x 1080"}};
    return list;
}

const std::vector<Option> &refresh_rates() {
    static const std::vector<Option> list = {{0, "Default (the console's)"}, {50, "50 Hz"}, {60, "60 Hz"}};
    return list;
}

const std::vector<Option> &widescreen_modes() {
    static const std::vector<Option> list = {{-1, "Default (the console's)"}, {0, "4:3"}, {1, "16:9"}};
    return list;
}

int resolution_of(const Settings::System &s) {
    if (!s.width || !s.height) return 0;
    if (s.width == 640 && s.height == 480) return 480;
    if (s.width == 1920 && s.height == 1080) return 1080;
    return s.width == 1280 && s.height == 720 ? 720 : -1; // a size the console has no mode of
}

void set_resolution(Settings::System &s, int height) {
    switch (height) {
    case 480: s.width = 640, s.height = 480; break;
    case 720: s.width = 1280, s.height = 720; break;
    case 1080: s.width = 1920, s.height = 1080; break;
    default: s.width = s.height = 0; break;
    }
}

size_t index_of(const std::vector<Option> &list, int value) {
    for (size_t i = 0; i < list.size(); i++)
        if (list[i].value == value) return i;
    return 0;
}

void clamp_system(Settings::System &s) {
    auto known = [](const std::vector<Option> &list, int v) {
        return std::any_of(list.begin(), list.end(), [&](const Option &c) { return c.value == v; });
    };
    if (!known(languages(), s.language)) s.language = 0;
    if (!known(countries(), s.country)) s.country = -1;
    if (!known(refresh_rates(), s.refresh)) s.refresh = 0;
    if (s.widescreen < -1 || s.widescreen > 1) s.widescreen = -1;
    if (!known(resolutions(), resolution_of(s))) s.width = s.height = 0; // (-1: not a mode)
}

} // namespace xf::frontend
