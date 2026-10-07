#pragma once
// The developer's switches: the SCMINER_* and BEDROCK_SHOT_* variables in
// the environment that turn tracing, dumps and measuring modes on. In a
// build with BEDROCK_DEV_TOOLS (cmake -DBEDROCK_DEV_TOOLS=ON) they are read
// from the environment; in a release build DEV_ENV() is a constant nullptr,
// the code behind each switch folds away, and the names are not in the
// binary. A release carries the app, not the workshop it was made with.
#ifdef BEDROCK_DEV_TOOLS
#include <cstdlib>
#define DEV_ENV(name) std::getenv(name)
#else
#define DEV_ENV(name) static_cast<const char*>(nullptr)
#endif
