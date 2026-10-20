#include "EmbeddedData.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace SCMiner {

bool embeddedResource(const std::string& name, const unsigned char** data, size_t* size) {
    const std::wstring wide(name.begin(), name.end());   // the names are plain ASCII
    HRSRC res = FindResourceW(nullptr, wide.c_str(), MAKEINTRESOURCEW(10) /*RT_RCDATA*/);
    if (!res) return false;
    HGLOBAL handle = LoadResource(nullptr, res);
    if (!handle) return false;
    *data = static_cast<const unsigned char*>(LockResource(handle));
    *size = SizeofResource(nullptr, res);
    return *data != nullptr && *size > 0;
}

}  // namespace SCMiner

#else

// The files are assembled straight into the binary. BEDROCK_RES_DIR and
// BEDROCK_TESTS_DIR come from CMake as absolute paths, so the assembler
// finds them whatever the build directory is; the list mirrors
// resources/bedrock.rc entry for entry, so both platforms carry the same.
#include <cstring>

#define BEDROCK_BLOB(sym, file)                                            \
    __asm__(".pushsection .rodata, \"a\", @progbits\n"                    \
            ".balign 16\n"                                                \
            ".global " #sym "_start\n" #sym "_start:\n"                    \
            ".incbin \"" file "\"\n"                                       \
            ".global " #sym "_end\n" #sym "_end:\n"                        \
            ".byte 0\n"                                                    \
            ".popsection\n");                                              \
    extern "C" const unsigned char sym##_start[], sym##_end[];

BEDROCK_BLOB(bedrock_blob_glyphs, BEDROCK_RES_DIR "/glyphs.bin")
BEDROCK_BLOB(bedrock_blob_glyphs_mfd, BEDROCK_RES_DIR "/glyphs_mfd.bin")
BEDROCK_BLOB(bedrock_blob_textline_model, BEDROCK_RES_DIR "/textline.onnx")
BEDROCK_BLOB(bedrock_blob_textline_keys, BEDROCK_RES_DIR "/textline_keys.txt")
BEDROCK_BLOB(bedrock_blob_selftest_prospector, BEDROCK_TESTS_DIR "/scanpanel/panel_prospector_1440.png")
BEDROCK_BLOB(bedrock_blob_selftest_mole, BEDROCK_TESTS_DIR "/scanpanel/panel_mole_cockpit_1440.png")
BEDROCK_BLOB(bedrock_blob_selftest_panel_1080, BEDROCK_TESTS_DIR "/scanpanel/scan_panel_1080_region.png")
BEDROCK_BLOB(bedrock_blob_selftest_turret, BEDROCK_TESTS_DIR "/scanpanel/panel_turret_stileron.png")
BEDROCK_BLOB(bedrock_blob_selftest_turret_volume, BEDROCK_TESTS_DIR "/scanpanel/panel_turret_copper.png")
BEDROCK_BLOB(bedrock_blob_selftest_golem, BEDROCK_TESTS_DIR "/scanpanel/panel_golem_long_1440.png")
BEDROCK_BLOB(bedrock_blob_selftest_loadout, BEDROCK_TESTS_DIR "/scanpanel/loadout_prospector_1440.jpg")
BEDROCK_BLOB(bedrock_blob_selftest_marker_lamp, BEDROCK_TESTS_DIR "/lamp_marker_3170.png")
BEDROCK_BLOB(bedrock_blob_selftest_marker_mfd, BEDROCK_TESTS_DIR "/mfd_marker_15420.png")
BEDROCK_BLOB(bedrock_blob_selftest_marker_canopy, BEDROCK_TESTS_DIR "/canopy_marker_60000.png")
// ...and the one thing bedrock.rc carries as an ICON rather than RCDATA:
// the window's icon, which Windows loads through LoadImage and the Linux
// window asks for by this name
BEDROCK_BLOB(bedrock_blob_appicon, BEDROCK_RES_DIR "/icon_256.png")
BEDROCK_BLOB(bedrock_blob_news_2_5, BEDROCK_RES_DIR "/news_2_5.jpg")
BEDROCK_BLOB(bedrock_blob_uex_powered, BEDROCK_RES_DIR "/uex_powered.png")
// ...and the faces the layout was measured in, or their metric twins: the
// window is laid out in Segoe UI and sets its figures in Bahnschrift, and
// neither is on a Linux machine. Selawik is Microsoft's own open Segoe UI
// stand-in (same metrics, SIL OFL); D-DIN is the DIN face Bahnschrift is
// (Datto, SIL OFL). Registered at start by the shim - see
// bedrock_shim::registerFonts - so the 61 MeasureString layouts come out
// where they do on Windows. Licences beside them in resources/fonts.
BEDROCK_BLOB(bedrock_blob_font_selawik, BEDROCK_RES_DIR "/fonts/selawk.ttf")
BEDROCK_BLOB(bedrock_blob_font_selawik_bold, BEDROCK_RES_DIR "/fonts/selawkb.ttf")
BEDROCK_BLOB(bedrock_blob_font_selawik_semibold, BEDROCK_RES_DIR "/fonts/selawksb.ttf")
BEDROCK_BLOB(bedrock_blob_font_selawik_light, BEDROCK_RES_DIR "/fonts/selawkl.ttf")
BEDROCK_BLOB(bedrock_blob_font_ddin, BEDROCK_RES_DIR "/fonts/D-DIN.ttf")
BEDROCK_BLOB(bedrock_blob_font_ddin_bold, BEDROCK_RES_DIR "/fonts/D-DIN-Bold.ttf")
BEDROCK_BLOB(bedrock_blob_font_ddin_condensed, BEDROCK_RES_DIR "/fonts/D-DINCondensed.ttf")
BEDROCK_BLOB(bedrock_blob_font_ddin_condensed_bold, BEDROCK_RES_DIR "/fonts/D-DINCondensed-Bold.ttf")

namespace SCMiner {

bool embeddedResource(const std::string& name, const unsigned char** data, size_t* size) {
    struct Entry { const char* name; const unsigned char* start; const unsigned char* end; };
    static const Entry table[] = {
        {"GLYPHS", bedrock_blob_glyphs_start, bedrock_blob_glyphs_end},
        {"GLYPHS_MFD", bedrock_blob_glyphs_mfd_start, bedrock_blob_glyphs_mfd_end},
        {"TEXTLINE_MODEL", bedrock_blob_textline_model_start, bedrock_blob_textline_model_end},
        {"TEXTLINE_KEYS", bedrock_blob_textline_keys_start, bedrock_blob_textline_keys_end},
        {"SELFTEST_PROSPECTOR", bedrock_blob_selftest_prospector_start, bedrock_blob_selftest_prospector_end},
        {"SELFTEST_MOLE", bedrock_blob_selftest_mole_start, bedrock_blob_selftest_mole_end},
        {"SELFTEST_PANEL_1080", bedrock_blob_selftest_panel_1080_start, bedrock_blob_selftest_panel_1080_end},
        {"SELFTEST_TURRET", bedrock_blob_selftest_turret_start, bedrock_blob_selftest_turret_end},
        {"SELFTEST_TURRET_VOLUME", bedrock_blob_selftest_turret_volume_start,
         bedrock_blob_selftest_turret_volume_end},
        {"SELFTEST_GOLEM", bedrock_blob_selftest_golem_start, bedrock_blob_selftest_golem_end},
        {"SELFTEST_LOADOUT", bedrock_blob_selftest_loadout_start, bedrock_blob_selftest_loadout_end},
        {"SELFTEST_MARKER_LAMP", bedrock_blob_selftest_marker_lamp_start, bedrock_blob_selftest_marker_lamp_end},
        {"SELFTEST_MARKER_MFD", bedrock_blob_selftest_marker_mfd_start, bedrock_blob_selftest_marker_mfd_end},
        {"SELFTEST_MARKER_CANOPY", bedrock_blob_selftest_marker_canopy_start, bedrock_blob_selftest_marker_canopy_end},
        {"NEWS_2_5", bedrock_blob_news_2_5_start, bedrock_blob_news_2_5_end},
        {"UEX_POWERED", bedrock_blob_uex_powered_start, bedrock_blob_uex_powered_end},
        {"APPICON", bedrock_blob_appicon_start, bedrock_blob_appicon_end},
        {"FONT_SELAWIK", bedrock_blob_font_selawik_start, bedrock_blob_font_selawik_end},
        {"FONT_SELAWIK_BOLD", bedrock_blob_font_selawik_bold_start, bedrock_blob_font_selawik_bold_end},
        {"FONT_SELAWIK_SEMIBOLD", bedrock_blob_font_selawik_semibold_start, bedrock_blob_font_selawik_semibold_end},
        {"FONT_SELAWIK_LIGHT", bedrock_blob_font_selawik_light_start, bedrock_blob_font_selawik_light_end},
        {"FONT_DDIN", bedrock_blob_font_ddin_start, bedrock_blob_font_ddin_end},
        {"FONT_DDIN_BOLD", bedrock_blob_font_ddin_bold_start, bedrock_blob_font_ddin_bold_end},
        {"FONT_DDIN_CONDENSED", bedrock_blob_font_ddin_condensed_start, bedrock_blob_font_ddin_condensed_end},
        {"FONT_DDIN_CONDENSED_BOLD", bedrock_blob_font_ddin_condensed_bold_start, bedrock_blob_font_ddin_condensed_bold_end},
    };
    for (const Entry& e : table) {
        if (name == e.name) {
            *data = e.start;
            *size = static_cast<size_t>(e.end - e.start);
            return *size > 0;
        }
    }
    return false;
}

}  // namespace SCMiner

#endif
