#pragma once

#include <QString>

namespace mixxx {

inline QString formatBeatCountdown(int beats) {
    if (beats <= 0) {
        return {};
    }
    return QStringLiteral("%1.%2").arg(beats / 4).arg(beats % 4);
}

} // namespace mixxx
