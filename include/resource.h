#pragma once
// Resource ids - shared by the .rc script and the window code
#define IDI_APPICON 101

// The version, in one place. The .rc needs four bare numbers for
// FILEVERSION and a string for the properties dialog; both are built from
// the four below, so a release is one edit and Windows cannot end up
// showing a different number than the app does. The fourth marks a
// package that replaces one of the same patch number, and is shown
// wherever it is not 0.
#define BEDROCK_VERSION_MAJOR 2
#define BEDROCK_VERSION_MINOR 6
#define BEDROCK_VERSION_PATCH 5
#define BEDROCK_VERSION_BUILD 1

#define BEDROCK_STR2(x) #x
#define BEDROCK_STR(x) BEDROCK_STR2(x)
#if BEDROCK_VERSION_BUILD > 0
#define BEDROCK_VERSION BEDROCK_STR(BEDROCK_VERSION_MAJOR) "." BEDROCK_STR(BEDROCK_VERSION_MINOR) \
                        "." BEDROCK_STR(BEDROCK_VERSION_PATCH) "." BEDROCK_STR(BEDROCK_VERSION_BUILD)
#define BEDROCK_VERSION_FULL BEDROCK_VERSION
#else
#define BEDROCK_VERSION BEDROCK_STR(BEDROCK_VERSION_MAJOR) "." BEDROCK_STR(BEDROCK_VERSION_MINOR) \
                        "." BEDROCK_STR(BEDROCK_VERSION_PATCH)
#define BEDROCK_VERSION_FULL BEDROCK_VERSION ".0"
#endif
#define BEDROCK_VERSION_W2(x) L##x
#define BEDROCK_VERSION_W1(x) BEDROCK_VERSION_W2(x)
#define BEDROCK_VERSION_W BEDROCK_VERSION_W1(BEDROCK_VERSION)

// The commit the build was made from, set by CMake from git: the full SHA,
// and the short one the ? window shows. "-dirty" when tracked files differed
// from that commit; "unknown" outside a git checkout.
#ifndef BEDROCK_GIT_SHA
#define BEDROCK_GIT_SHA "unknown"
#endif
#ifndef BEDROCK_GIT_SHORT
#define BEDROCK_GIT_SHORT "unknown"
#endif
#define BEDROCK_GIT_SHORT_W BEDROCK_VERSION_W1(BEDROCK_GIT_SHORT)

// Where the app looks for a newer version: a text file of key=value
// lines (see Updater.h). Set by the build for official releases only
// (cmake -DBEDROCK_UPDATE_URL=https://...); a build without it never looks,
// so a changed build is not replaced by the official one. updateurl= in
// region_config.txt overrides it.
#ifndef BEDROCK_UPDATE_URL
#define BEDROCK_UPDATE_URL ""
#endif
