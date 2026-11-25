#pragma once
// ResponseHeaders.h
// Which X-Visivo-* response headers the client reads and when one is MISSING.
//
// The backend sends X-Visivo-Bunit and X-Visivo-Spectral-Axis-Unit empty for a
// file with no such unit. An empty header is still there: the client read an
// empty value as an absent header (QByteArray::isNull) and logged contract
// drift on every open of such a file.

#include "BackendHeaders.h"

#include <QByteArray>
#include <QMap>
#include <QSet>

namespace visivo::responseheaders {

/// Names whose absence in a binary-endpoint response means the client/server
/// contract has drifted (not merely an optional field): the geometry that
/// sizes the receive buffers, and the scientific units the client displays.
inline bool isRequired(const char *name)
{
    static const QSet<QByteArray> kRequired = {
        QByteArray(VisivoHeaders::WIDTH),
        QByteArray(VisivoHeaders::HEIGHT),
        QByteArray(VisivoHeaders::DEPTH),
        QByteArray(VisivoHeaders::BUNIT),
        QByteArray(VisivoHeaders::SPECTRAL_AXIS_UNIT),
    };
    return kRequired.contains(QByteArray(name));
}

/// Whether a required header is absent from @p headers (keys lower-cased, as
/// performBinaryPost stores them). Present with an empty value is present.
inline bool isMissingRequired(const QMap<QByteArray, QByteArray> &headers, const char *name)
{
    return isRequired(name) && !headers.contains(QByteArray(name).toLower());
}

}   // namespace visivo::responseheaders
