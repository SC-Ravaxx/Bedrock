cmake_minimum_required(VERSION 3.20)
project(StarCitizenMiner VERSION 2.6.5.2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The developer's workshop - the harvest, bench and probe commands in
# main.cpp, the SCMINER_* switches, the rock look log - is compiled in only
# when asked for. A release carries the app and nothing of how it was made:
#     cmake -S . -B build -DBEDROCK_DEV_TOOLS=ON      the workshop
#     cmake -S . -B build-release                     what ships
option(BEDROCK_DEV_TOOLS "Compile the developer tools and switches in" OFF)
if(BEDROCK_DEV_TOOLS)
    add_compile_definitions(BEDROCK_DEV_TOOLS=1)
endif()

# Tesseract reads nothing in a release: the signature is read by the glyph
# reader and the second reader, and Tesseract is a developer switch
# (SCMINER_OCR) that cannot start without tessdata, which the package does
# not carry. So only the workshop links it; a release leaves it out, and
# with it nine DLLs (13 MB, libcurl and OpenSSL among them) that nothing
# else needs.
option(BEDROCK_WITH_TESSERACT "Link Tesseract - the developer tools' OCR" ${BEDROCK_DEV_TOOLS})
if(BEDROCK_WITH_TESSERACT)
    add_compile_definitions(BEDROCK_TESSERACT=1)
endif()

# Where the app looks for a newer version (an https address of a
# bedrock-latest.txt, see include/resource.h). Official releases set it; a
# build without it never checks for updates.
set(BEDROCK_UPDATE_URL "" CACHE STRING "Update manifest address (official builds only; empty = no update check)")

# The commit the build is made from, so a binary names the exact source it
# came from (shown in the ? window and session.txt, checked by
# tools/make_release.py). "-dirty" when tracked files differ from the commit.
# Read again whenever HEAD or the index changes.
set(BEDROCK_GIT_SHA "unknown")
set(BEDROCK_GIT_SHORT "unknown")
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(COMMAND "${GIT_EXECUTABLE}" -c safe.directory=* rev-parse HEAD
                    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" RESULT_VARIABLE BEDROCK_GIT_RC
                    OUTPUT_VARIABLE BEDROCK_GIT_HEAD OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(BEDROCK_GIT_RC EQUAL 0 AND BEDROCK_GIT_HEAD MATCHES "^[0-9a-f]+$")
        execute_process(COMMAND "${GIT_EXECUTABLE}" -c safe.directory=* status --porcelain --untracked-files=no
                        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
                        OUTPUT_VARIABLE BEDROCK_GIT_CHANGES OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        string(SUBSTRING "${BEDROCK_GIT_HEAD}" 0 7 BEDROCK_GIT_SHORT)
        set(BEDROCK_GIT_SHA "${BEDROCK_GIT_HEAD}")
        if(BEDROCK_GIT_CHANGES)
            string(APPEND BEDROCK_GIT_SHA "-dirty")
            string(APPEND BEDROCK_GIT_SHORT "-dirty")
        endif()
        execute_process(COMMAND "${GIT_EXECUTABLE}" -c safe.directory=* rev-parse --absolute-git-dir
                        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
                        OUTPUT_VARIABLE BEDROCK_GIT_DIR OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        execute_process(COMMAND "${GIT_EXECUTABLE}" -c safe.directory=* symbolic-ref -q HEAD
                        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
                        OUTPUT_VARIABLE BEDROCK_GIT_REF OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        foreach(watched HEAD index packed-refs ${BEDROCK_GIT_REF})
            if(EXISTS "${BEDROCK_GIT_DIR}/${watched}")
                set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${BEDROCK_GIT_DIR}/${watched}")
            endif()
        endforeach()
    endif()
endif()
message(STATUS "Bedrock is built from commit ${BEDROCK_GIT_SHA}")

# Reproducible builds: the same commit, built with the same tools, gives the
# same binary byte for byte. MSVC stamps the time into every object and the
# exe unless /Brepro says otherwise; GCC writes the source folder into
# __FILE__, which -ffile-prefix-map takes out, so where the checkout lies does
# not matter either.
if(MSVC)
    add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:/Brepro>")
    add_link_options("/Brepro")
    # __FILE__ (OpenCV's CV_Assert carries it) without the checkout's folder
    file(TO_NATIVE_PATH "${CMAKE_SOURCE_DIR}" BEDROCK_SOURCE_NATIVE)
    add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:/d1trimfile:${BEDROCK_SOURCE_NATIVE}>")
else()
    add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:-ffile-prefix-map=${CMAKE_SOURCE_DIR}/=>"
                        "$<$<COMPILE_LANGUAGE:CXX>:-ffile-prefix-map=${CMAKE_BINARY_DIR}/=build/>")
endif()
add_compile_definitions("$<$<COMPILE_LANGUAGE:CXX>:BEDROCK_GIT_SHA=\"${BEDROCK_GIT_SHA}\">"
                        "$<$<COMPILE_LANGUAGE:CXX>:BEDROCK_GIT_SHORT=\"${BEDROCK_GIT_SHORT}\">")
if(BEDROCK_UPDATE_URL)
    add_compile_definitions("$<$<COMPILE_LANGUAGE:CXX>:BEDROCK_UPDATE_URL=\"${BEDROCK_UPDATE_URL}\">")
endif()

# Find required packages
find_package(OpenCV REQUIRED)
if(WIN32)
    if(BEDROCK_WITH_TESSERACT)
        find_package(Tesseract CONFIG REQUIRED)
    endif()
    # zstd: Data.p4k packs its files with it (P4kReader). Tesseract's tiff
    # brings it along too.
    find_package(zstd CONFIG REQUIRED)
else()
    find_package(PkgConfig REQUIRED)
    if(BEDROCK_WITH_TESSERACT)
        pkg_check_modules(TESSERACT REQUIRED tesseract)
    endif()
    pkg_check_modules(ZSTD REQUIRED libzstd)
    pkg_check_modules(X11 REQUIRED x11 xcomposite xext xfixes)
endif()

# Include directories
include_directories(${OpenCV_INCLUDE_DIRS})
include_directories(${CMAKE_SOURCE_DIR}/include)

# The Ore Finder's prices come from UEX (uexcorp.space). Asked with Bedrock's
# own app token, UEX knows which app is asking; without one it answers all
# the same, but takes no reports. The token is uex_token.txt beside this file
# - never committed (.gitignore). It does not go into the program: it is
# written, mixed byte by byte, into uex_token.dat beside it, which a release
# carries. So the program is the same with or without a token, and anyone can
# build the official binary from the same commit (README.md, "Reproducing a
# release"). It identifies the app, not a player: it reads public prices and
# goes with the price reports and refinery jobs a player chooses to send
# under their own key (uex_key.dat, which no build carries). A token changed
# in the file is read at the next build.
#
# Read as bytes, so that what an editor puts around the token is not taken
# for part of it: a byte order mark in front, white space and line ends on
# either side. A token is one word of printable characters; a file holding
# anything more ("Bearer ...", a second line) is left out with a warning, and
# that build asks anonymously - better than a token UEX would turn away.
set(UEX_TOKEN_HEX "")
if(EXISTS "${CMAKE_SOURCE_DIR}/uex_token.txt")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/uex_token.txt")
    file(READ "${CMAKE_SOURCE_DIR}/uex_token.txt" UEX_TOKEN_HEX HEX)
    string(REGEX REPLACE "^efbbbf" "" UEX_TOKEN_HEX "${UEX_TOKEN_HEX}")
    # two hex digits to a byte: a match from the front, or one that runs to
    # the end, always falls on whole bytes
    string(REGEX REPLACE "^(20|09|0a|0d)+" "" UEX_TOKEN_HEX "${UEX_TOKEN_HEX}")
    string(REGEX REPLACE "(20|09|0a|0d)+$" "" UEX_TOKEN_HEX "${UEX_TOKEN_HEX}")
    string(REGEX MATCHALL ".." UEX_TOKEN_BYTES "${UEX_TOKEN_HEX}")
    foreach(byte IN LISTS UEX_TOKEN_BYTES)
        math(EXPR value "0x${byte}")
        if(value LESS 33 OR value GREATER 126)
            message(WARNING "uex_token.txt holds more than the token alone - left out; this build asks UEX anonymously")
            set(UEX_TOKEN_HEX "")
            break()
        endif()
    endforeach()
endif()
string(LENGTH "${UEX_TOKEN_HEX}" UEX_TOKEN_DIGITS)
math(EXPR UEX_TOKEN_LENGTH "${UEX_TOKEN_DIGITS} / 2")
# uex_token.dat: one line of decimal numbers, each byte of the token mixed
# the way UexReports::unmixAppToken takes off again
set(UEX_TOKEN_FILE "${CMAKE_BINARY_DIR}/uex_token.dat")
if(UEX_TOKEN_LENGTH GREATER 0)
    set(UEX_TOKEN_MIXED "")
    math(EXPR UEX_TOKEN_LAST "${UEX_TOKEN_LENGTH} - 1")
    foreach(i RANGE ${UEX_TOKEN_LAST})
        math(EXPR at "${i} * 2")
        string(SUBSTRING "${UEX_TOKEN_HEX}" ${at} 2 byte)
        math(EXPR mixed "0x${byte} ^ ((${i} * 37 + 91) % 256)")
        if(i GREATER 0)
            string(APPEND UEX_TOKEN_MIXED ",")
        endif()
        string(APPEND UEX_TOKEN_MIXED "${mixed}")
    endforeach()
    file(WRITE "${UEX_TOKEN_FILE}" "${UEX_TOKEN_MIXED}\n")
else()
    file(REMOVE "${UEX_TOKEN_FILE}")
endif()

# The MSVC spellings the project uses in 33 places - _stricmp, localtime_s and
# their like - translated in one header rather than rewritten at every call.
# Forced in, so no source file has to carry an include for it.
if(NOT WIN32)
    add_compile_options(-include ${CMAKE_SOURCE_DIR}/include/Portability.h)
endif()

# --------------------------------------------------------------------------
# The core: everything that describes the game and reads it, with no window
# system under it. It builds on both platforms from the same sources.
# --------------------------------------------------------------------------
set(CORE_SOURCES
    src/GlyphReader.cpp
    src/EmbeddedData.cpp
    src/TextLineReader.cpp
    src/SignatureAnalyzer.cpp
    src/OCREngine.cpp
    src/GameState.cpp
    src/GamePlaces.cpp
    src/ScanPanel.cpp
    src/LoadoutReader.cpp
    src/RockContents.cpp
    src/RefineryTerminal.cpp
    src/RefineryOrders.cpp
    src/TradeBook.cpp
    src/BlueprintLibrary.cpp
    src/Reputation.cpp
    src/NoteBook.cpp
    src/DataBackup.cpp
    src/AppPaths.cpp
    src/P4kReader.cpp
    src/GameTexts.cpp
    src/MiniJson.cpp
    src/UexPrices.cpp
    src/UexReports.cpp
    src/UexMarket.cpp
)

# Source files
set(SOURCES
    src/main.cpp
    src/ScreenCapture.cpp
    src/DxgiCapture.cpp
    src/FrameRecorder.cpp
    src/Application.cpp
    src/MinerGUI.cpp
    src/Updater.cpp
    src/DevTools.cpp
    src/DevShots.cpp
    src/SelfTest.cpp
    ${CORE_SOURCES}
)

set(HEADERS
    include/ScreenCapture.h
    include/DxgiCapture.h
    include/OCREngine.h
    include/GlyphReader.h
    include/TextLineReader.h
    include/FrameRecorder.h
    include/SignatureAnalyzer.h
    include/Application.h
    include/MiningData.h
    include/MinerGUI.h
    include/BlueprintLibrary.h
    include/Reputation.h
    include/GameState.h
    include/GamePlaces.h
    include/NoteBook.h
    include/DataBackup.h
    include/Portability.h
    include/P4kReader.h
    include/GameTexts.h
    include/GameTextMarks.h
    include/LoadoutReader.h
    include/RockContents.h
    include/RefineryTerminal.h
    include/RefineryOrders.h
    include/TradeBook.h
    include/CommodityData.h
    include/MiniJson.h
    include/UexPrices.h
    include/UexReports.h
    include/UexMarket.h
)

if(WIN32)
    # Create executable (WIN32 for a GUI application)
    enable_language(RC)
    add_executable(${PROJECT_NAME} WIN32 ${SOURCES} ${HEADERS} resources/bedrock.rc)
    set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "Bedrock")

    # Source files are UTF-8 (without BOM) - required for the unicode UI strings
    if(MSVC)
        target_compile_options(${PROJECT_NAME} PRIVATE /utf-8)
    endif()

    # The app token beside the exe, where it is looked for (see above)
    if(EXISTS "${UEX_TOKEN_FILE}")
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${UEX_TOKEN_FILE}" "$<TARGET_FILE_DIR:${PROJECT_NAME}>/uex_token.dat")
    endif()

    target_link_libraries(${PROJECT_NAME} PRIVATE
        ${OpenCV_LIBS}
        zstd::libzstd
        gdi32
        gdiplus
        user32
        comctl32
        dwmapi
        d3d11
        dxgi
        comdlg32
        shell32
        ole32
    )
    if(BEDROCK_WITH_TESSERACT)
        target_link_libraries(${PROJECT_NAME} PRIVATE Tesseract::libtesseract)
    endif()
else()
    # ----------------------------------------------------------------------
    # Linux. The core builds as a library so the pieces above it - the capture
    # layer, and in time the window - can be added one at a time and each one
    # measured against the Windows build rather than assumed.
    # ----------------------------------------------------------------------
    add_library(bedrock_core STATIC ${CORE_SOURCES})
    target_include_directories(bedrock_core PUBLIC
        ${CMAKE_SOURCE_DIR}/include
        ${OpenCV_INCLUDE_DIRS}
        ${ZSTD_INCLUDE_DIRS}
    )
    target_link_libraries(bedrock_core PUBLIC ${OpenCV_LIBS} ${ZSTD_LIBRARIES})
    target_link_directories(bedrock_core PUBLIC ${ZSTD_LIBRARY_DIRS})
    # by name only when asked for: pkg-config's answers stay in the cache of
    # a build folder that once had them, and would be linked in regardless
    if(BEDROCK_WITH_TESSERACT)
        target_include_directories(bedrock_core PUBLIC ${TESSERACT_INCLUDE_DIRS})
        target_link_libraries(bedrock_core PUBLIC ${TESSERACT_LIBRARIES})
        target_link_directories(bedrock_core PUBLIC ${TESSERACT_LIBRARY_DIRS})
    endif()
    # what the binary carries inside itself - see src/EmbeddedData.cpp
    target_compile_definitions(bedrock_core PRIVATE
        BEDROCK_RES_DIR="${CMAKE_SOURCE_DIR}/resources"
        BEDROCK_TESTS_DIR="${CMAKE_SOURCE_DIR}/tests")
    # .incbin reads those files while the object is assembled, out of make's
    # sight, so a changed file would not be picked up. Named here, a changed one
    # rebuilds the object. Read off EmbeddedData.cpp itself, so its list
    # stays the only one; a line added there runs cmake again.
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/EmbeddedData.cpp")
    file(STRINGS "${CMAKE_SOURCE_DIR}/src/EmbeddedData.cpp" BEDROCK_BLOB_LINES REGEX "^BEDROCK_BLOB\\(")
    set(BEDROCK_EMBEDDED "")
    foreach(line IN LISTS BEDROCK_BLOB_LINES)
        if(line MATCHES "BEDROCK_RES_DIR \"([^\"]+)\"")
            list(APPEND BEDROCK_EMBEDDED "${CMAKE_SOURCE_DIR}/resources${CMAKE_MATCH_1}")
        elseif(line MATCHES "BEDROCK_TESTS_DIR \"([^\"]+)\"")
            list(APPEND BEDROCK_EMBEDDED "${CMAKE_SOURCE_DIR}/tests${CMAKE_MATCH_1}")
        endif()
    endforeach()
    set_source_files_properties(src/EmbeddedData.cpp PROPERTIES OBJECT_DEPENDS "${BEDROCK_EMBEDDED}")

    # Reading the screen, the way it has to be done under X11: the game's
    # window is found in the window tree and its pixels come out of its
    # Composite pixmap. The root window is not readable under XWayland - a
    # capture from it comes back black - so the window is addressed directly.
    # ScreenCapture.cpp builds on both: its first 170 lines are the Win32
    # capture and sit in an #ifdef, the remaining 1,200 are the locator - the
    # plate search, the channel splitting, the probe. Shared rather than
    # copied, so a fix to the locator reaches both platforms at once.
    add_library(bedrock_capture STATIC src/LinuxCapture.cpp src/ScreenCapture.cpp src/DevTools.cpp)
    target_include_directories(bedrock_capture PUBLIC
        ${CMAKE_SOURCE_DIR}/include
        ${OpenCV_INCLUDE_DIRS}
        ${X11_INCLUDE_DIRS}
    )
    target_link_libraries(bedrock_capture PUBLIC bedrock_core ${X11_LIBRARIES})
    target_link_directories(bedrock_capture PUBLIC ${X11_LIBRARY_DIRS})

    add_executable(bedrock-read src/linux_cli.cpp)
    target_link_libraries(bedrock-read PRIVATE bedrock_capture)

    # The window, step one: does MinerGUI's drawing code run on QPainter as it
    # stands? GdiplusShim.h answers to the ten GDI+ calls the app actually
    # makes, and the demo draws a card with helpers copied verbatim out of
    # MinerGUI.cpp. Built only where Qt is present - the reader and the
    # capture layer do not need it.
    find_package(Qt6 QUIET COMPONENTS Core Gui Widgets Network)
    if(Qt6_FOUND)
        add_executable(bedrock-shim-demo src/linux_gui_demo.cpp)
        # against the real core, so the figures in the picture come from the
        # real analyzer rather than being made up for the demo
        target_link_libraries(bedrock-shim-demo PRIVATE bedrock_core Qt6::Core Qt6::Gui)
        target_include_directories(bedrock-shim-demo PRIVATE ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/include/win32)
        target_compile_definitions(bedrock-shim-demo PRIVATE QT_NO_KEYWORDS)

        # The window itself: MinerGUI.cpp as it stands, compiled against the
        # Win32 shim (include/win32/ answers to <Windows.h>, <gdiplus.h> and
        # the rest; include/Win32Shim.h says what it covers). Nothing of the
        # Windows build is touched by it - the shim is only on this target's
        # include path.
        add_executable(bedrock
            src/linux/linux_app.cpp
            src/Application.cpp
            src/MinerGUI.cpp
            src/Updater.cpp
            src/FrameRecorder.cpp
            src/DevShots.cpp
            src/SelfTest.cpp
            src/linux/Win32Shim.cpp
            src/linux/Win32Controls.cpp
            src/linux/X11Hotkeys.cpp
        )
        target_include_directories(bedrock BEFORE PRIVATE
            ${CMAKE_SOURCE_DIR}/include/win32
            ${CMAKE_SOURCE_DIR}/include
            ${CMAKE_SOURCE_DIR}/src/linux
        )
        # xcb: the hotkey filter reads the key events off Qt's own X connection
        find_library(XCB_LIBRARY xcb)
        target_link_libraries(bedrock PRIVATE bedrock_capture Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network
                              ${XCB_LIBRARY})
        # Qt's signals/slots/emit macros would collide with words MinerGUI
        # uses for its own; the Q_ spellings do the same job
        target_compile_definitions(bedrock PRIVATE QT_NO_KEYWORDS)
        target_compile_options(bedrock PRIVATE -Wno-unknown-pragmas)
    else()
        message(STATUS "Qt6 not found - skipping the GDI+ shim demo and the window")
    endif()
endif()

# Copy Tesseract data if needed
if(EXISTS "${CMAKE_SOURCE_DIR}/tessdata")
    file(COPY ${CMAKE_SOURCE_DIR}/tessdata DESTINATION ${CMAKE_BINARY_DIR})
endif()

# Digit templates for the OCR-free reader
if(EXISTS "${CMAKE_SOURCE_DIR}/glyphs")
    file(COPY ${CMAKE_SOURCE_DIR}/glyphs DESTINATION ${CMAKE_BINARY_DIR})
endif()
if(EXISTS "${CMAKE_SOURCE_DIR}/glyphs_mfd")
    file(COPY ${CMAKE_SOURCE_DIR}/glyphs_mfd DESTINATION ${CMAKE_BINARY_DIR})
endif()
