#pragma once
#include <string>
// Executable-memory feasibility probe (the JIT prerequisite). Returns a human-readable report (UTF-8).
// Pure C++ + SEH, compiled separately (not /ZW).
std::string RunJitProbe();
