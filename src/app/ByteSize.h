#pragma once

// ByteSize.h
// A byte count as the dialogs word it: KiB, MiB or GiB, the way a file
// manager does. One helper for every size estimate shown before a download or
// an export (the AREPO extraction, the PLUTO Cartesian view), so they read alike.

#include <QString>

namespace visivo {

inline QString formatBytes(qint64 bytes)
{
    constexpr double kGiB = 1024.0 * 1024.0 * 1024.0;
    constexpr double kMiB = 1024.0 * 1024.0;
    if (bytes >= kGiB)
        return QStringLiteral("%1 GiB").arg(bytes / kGiB, 0, 'f', 2);
    if (bytes >= kMiB)
        return QStringLiteral("%1 MiB").arg(bytes / kMiB, 0, 'f', 1);
    return QStringLiteral("%1 KiB").arg(bytes / 1024.0, 0, 'f', 0);
}

}   // namespace visivo
