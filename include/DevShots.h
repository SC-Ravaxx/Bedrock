#pragma once
// The screenshot run - see src/DevShots.cpp. Compiled into a developer
// build on Windows (-DBEDROCK_DEV_TOOLS=ON) and into the Linux window.
#include <string>

// Photographs the windows into <dir>: main.png, overlay.png, planner.png and
// the rest. The windows are created outside every monitor and never
// activated, so nothing shows on the desktop; the picture comes from the
// window's own composition surface (PrintWindow with PW_RENDERFULLCONTENT),
// or is painted straight into a bitmap for the windows DWM does not compose
// off-screen.
int runScreenshots(const std::string& dir);
