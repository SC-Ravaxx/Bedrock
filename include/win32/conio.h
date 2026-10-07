#pragma once
// Linux: the two conio calls Application's console mode makes, on termios.
// _kbhit says whether a key is waiting, _getch takes it without echo - the
// terminal is switched out of line mode for the one read and put back.
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

namespace bedrock_conio {
struct RawMode {
    termios saved{};
    bool on = false;
    RawMode() {
        if (!isatty(STDIN_FILENO) || tcgetattr(STDIN_FILENO, &saved) != 0) return;
        termios raw = saved;
        raw.c_lflag &= ~(ICANON | ECHO);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        on = tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0;
    }
    ~RawMode() {
        if (on) tcsetattr(STDIN_FILENO, TCSANOW, &saved);
    }
};
}  // namespace bedrock_conio

inline int _kbhit() {
    bedrock_conio::RawMode raw;
    fd_set set;
    FD_ZERO(&set);
    FD_SET(STDIN_FILENO, &set);
    timeval none{0, 0};
    return select(STDIN_FILENO + 1, &set, nullptr, nullptr, &none) > 0 ? 1 : 0;
}

inline int _getch() {
    bedrock_conio::RawMode raw;
    unsigned char c = 0;
    if (read(STDIN_FILENO, &c, 1) != 1) return -1;
    return c;
}
