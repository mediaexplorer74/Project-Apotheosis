// SmokeTest.h — Phase 0 check: run "1+1" via the JSC C API and return the result string.
// Depends only on JavaScriptCore's C API (JavaScript.h, pure C), so it links against the
// clang-cl-built .lib cleanly.
#pragma once
#include <string>

// Sets JSC single-threaded GC at runtime (avoids SuspendThread/GetThreadContext in the
// App Container). Returns a result such as "1+1 = 2", or an error message.
std::wstring RunJscSmokeTest();
