#pragma once

#include <cstddef>

namespace mixxxtest {
/// Test-only thread-local C++ new/delete accounting. Does not instrument C
/// allocators inside shared libraries, locks, I/O, or audio-device deadlines.
extern thread_local bool countCallbackAllocations;
extern thread_local std::size_t callbackAllocations;
extern thread_local std::size_t callbackDeallocations;
} // namespace mixxxtest
