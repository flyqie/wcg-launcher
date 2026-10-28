#pragma once

#include <ostream>

// Console output, enabled by LAUNCHER_DEBUG=1. Everything the program prints goes
// through here and is silent unless asked for; failures a caller has to act on are
// return values and exit codes instead.
std::ostream& Debug();

// Same, for the few places that have wide strings to print.
std::wostream& DebugW();

// True when LAUNCHER_DEBUG is set to 1, y or t (any case).
bool DebugEnabled();
