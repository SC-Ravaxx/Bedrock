#pragma once
// The handful of MSVC names the project uses, mapped for every platform
// but Windows. None of the calls does anything Windows-only; the functions
// are only named differently elsewhere (localtime_s also swaps its
// arguments). Translating them once here leaves the Windows code as it is.
//
// Included with -include, so no source file has to change.

#ifndef _WIN32

#include <cstring>
#include <ctime>
#include <cwchar>

inline int _stricmp(const char* a, const char* b) {
    return ::strcasecmp(a, b);
}

inline int _strnicmp(const char* a, const char* b, size_t n) {
    return ::strncasecmp(a, b, n);
}

inline int _wcsicmp(const wchar_t* a, const wchar_t* b) {
    return ::wcscasecmp(a, b);
}

// The argument order is reversed:
//   MSVC   errno_t localtime_s(struct tm* out, const time_t* t)
//   POSIX  struct tm* localtime_r(const time_t* t, struct tm* out)
// They also report success differently; this returns 0 on success.
inline int localtime_s(struct tm* out, const time_t* t) {
    return ::localtime_r(t, out) ? 0 : 1;
}

inline int gmtime_s(struct tm* out, const time_t* t) {
    return ::gmtime_r(t, out) ? 0 : 1;
}

// sscanf_s takes a buffer size after the pointer for %s and %c; sscanf
// does not. Safe only while every caller scans numbers: a %s would be
// silently wrong.
#define sscanf_s sscanf

// The _s array copiers MinerGUI.cpp uses: the size comes from the array,
// and _TRUNCATE cuts the string short instead of failing.
#include <cstddef>
#include <cstdarg>
#include <cwchar>
#include <string>
#define _TRUNCATE ((size_t)-1)
template <size_t N>
inline int wcscpy_s(wchar_t (&dst)[N], const wchar_t* src) {
    const size_t n = std::wcslen(src);
    if (n + 1 > N) { dst[0] = 0; return 34; }   // ERANGE
    std::wmemcpy(dst, src, n + 1);
    return 0;
}
template <size_t N>
inline int wcsncpy_s(wchar_t (&dst)[N], const wchar_t* src, size_t count) {
    size_t n = std::wcslen(src);
    if (count != _TRUNCATE && n > count) n = count;
    if (n >= N) n = N - 1;
    std::wmemcpy(dst, src, n);
    dst[n] = 0;
    return 0;
}

// swprintf: in a wide format MSVC reads %s as a wide string, glibc as a
// narrow one, so under Linux a wchar_t buffer is read as char* and crashes.
// The callers keep the MSVC meaning; the format is rewritten to %ls and %lc
// here before it goes to vswprintf. The source calls std::swprintf, so std
// gets the name too, and the macro routes both spellings here.
inline int bedrock_swprintf_fixed(wchar_t* buffer, size_t count, const wchar_t* format, ...) {
    std::wstring fixed;
    for (const wchar_t* p = format; *p; ++p) {
        fixed.push_back(*p);
        if (*p != L'%') continue;
        ++p;
        if (*p == L'%') { fixed.push_back(*p); continue; }
        // Copy flags, width and precision, then look at the conversion
        while (*p && std::wcschr(L"-+ #0123456789.*", *p)) fixed.push_back(*p++);
        if (*p == L's' || *p == L'c') { fixed.push_back(L'l'); fixed.push_back(*p); }
        else if (*p == L'S') fixed.push_back(L's');   // MSVC: %S = narrow
        else if (*p) fixed.push_back(*p);
        else break;
    }
    std::va_list args;
    va_start(args, format);
    const int n = std::vswprintf(buffer, count, fixed.c_str(), args);
    va_end(args);
    return n;
}
#define swprintf bedrock_swprintf_fixed
namespace std { using ::bedrock_swprintf_fixed; }

#endif  // !_WIN32
