// bedrock - the app on Linux.
//
// The same Application the Windows build runs, on the Win32 shim (see
// include/Win32Shim.h and docs/linux-port.md): the window, the scan, the log
// reader, the updater's check. Reading the game's picture goes through
// LinuxCapture, which ScreenCapture stands in front of on this platform.
//
//   bedrock                    the window
//   bedrock --console          the console mode, no window
//   bedrock --screenshot <dir> every window photographed into <dir>, then quit
//   bedrock --selftest         the program's checks on itself
#include "AppPaths.h"
#include "Application.h"
#include "DevShots.h"
#include "EmbeddedData.h"
#include "SelfTest.h"
#include "Win32Shim.h"

#include <QApplication>
#include <QImage>

#include <clocale>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <unistd.h>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("Bedrock");
    // Qt sets the C library's locale from the environment on Unix, and with
    // it the decimal comma of a German or French system: atof("64.16") is 64
    // there, and every figure Bedrock reads, writes or parses (the panel's
    // instability and SCU, a crafted laser's per cent in loadout.txt, a
    // trade's amount) would lose what stands after its point. The numbers are
    // the C locale's; Qt's own formatting goes by QLocale and is not touched.
    std::setlocale(LC_NUMERIC, "C");

    // The working directory is the program's own folder, as on Windows, so
    // that tessdata/ and glyphs/ beside the binary are found by their
    // relative names
    {
        std::error_code ec;
        const std::filesystem::path self = std::filesystem::read_symlink("/proc/self/exe", ec);
        if (!ec && !self.empty()) std::filesystem::current_path(self.parent_path(), ec);
    }
    SCMiner::AppPaths::migrateFromProgramFolder();

    // The faces the layout was measured in, out of the program itself
    bedrock_shim::registerFonts();

    // The icon the exe carries as a resource on Windows
    {
        const unsigned char* data = nullptr;
        size_t size = 0;
        if (SCMiner::embeddedResource("APPICON", &data, &size)) {
            QImage icon;
            if (icon.loadFromData(data, (int)size)) bedrock_shim::setApplicationIcon(icon);
        }
    }

    bool console = false;
    bool selfTest = false;
    std::string shots;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--console") == 0) console = true;
        else if (std::strcmp(argv[i], "--selftest") == 0 || std::strcmp(argv[i], "--test") == 0) selfTest = true;
        else if (std::strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) shots = argv[++i];
        else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            std::cout << "bedrock [--console] [--selftest] [--screenshot <dir>]" << std::endl;
            return 0;
        }
    }
    if (!shots.empty()) return runScreenshots(shots);
    if (selfTest) return runSelfTest();

    SCMiner::Application bedrock;
    if (!bedrock.initialize()) {
        std::cerr << "Initialization failed." << std::endl;
        return 1;
    }
    if (console) bedrock.run();
    else bedrock.runWithGUI(nullptr);
    return 0;
}
