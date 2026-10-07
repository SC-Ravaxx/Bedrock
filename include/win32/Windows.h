#pragma once
// Linux: the Win32 shim answers to this header - see include/Win32Shim.h
#include "Win32Shim.h"

// The words the real <windows.h> takes for itself through rpcndr.h and
// windef.h, defined the same way here so that a name MSVC cannot compile
// (a parameter called `small`, say) fails on Linux first.
#ifndef small
#define small char
#endif
#ifndef hyper
#define hyper __int64
#endif
#ifndef interface
#define interface struct
#endif
