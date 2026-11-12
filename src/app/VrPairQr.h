#pragma once

// VrPairQr.h
// The QR code of the pairing page's address (issue #214), as a square of
// dark/light modules, for the dialog to draw. The encoder is Project Nayuki's
// QR Code generator (MIT), vendored in src/thirdparty/qrcodegen.
//
// Widget-free: unit-tested in tests/test_vr_pair.cpp.

#include "thirdparty/qrcodegen/qrcodegen.hpp"

#include <QList>
#include <QString>

namespace VrPairQr {

struct Matrix
{
    int size{ 0 };            ///< modules per side, 0 when there is nothing to draw
    QList<bool> dark;         ///< row-major, size × size

    bool at(int x, int y) const { return dark.at(y * size + x); }
};

/// @p text as a QR code, error correction M (a phone camera reads it off a
/// screen at an angle) or better when that fits in the same size. Empty for
/// empty text or text too long for any QR code.
inline Matrix encode(const QString &text)
{
    Matrix m;
    if (text.isEmpty())
        return m;
    try {
        const QByteArray utf8 = text.toUtf8();
        const qrcodegen::QrCode qr =
                qrcodegen::QrCode::encodeText(utf8.constData(), qrcodegen::QrCode::Ecc::MEDIUM);
        m.size = qr.getSize();
        m.dark.reserve(m.size * m.size);
        for (int y = 0; y < m.size; ++y)
            for (int x = 0; x < m.size; ++x)
                m.dark.append(qr.getModule(x, y));
    } catch (const qrcodegen::data_too_long &) {
        m = Matrix();
    }
    return m;
}

}   // namespace VrPairQr
