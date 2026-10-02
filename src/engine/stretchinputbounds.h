#pragma once

#include "util/defs.h"

namespace mixxx::engine::stretch {

// Shared stretch-input storage and per-callback read limits. Total attempts
// include short, zero-length and unavailable reads. The zero-read limit counts
// successful zero-length loop transitions; unavailable reads return immediately.
// Neither per-callback limit bounds the lifetime of a partially collected batch.
inline constexpr int kMaxInputFrames = MAX_BUFFER_LEN;
inline constexpr int kReadAttemptBudget = 4096;
inline constexpr int kZeroReadAttemptBudget = 64;

} // namespace mixxx::engine::stretch
