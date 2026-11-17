#ifndef REGION_FLUX_UI_H
#define REGION_FLUX_UI_H
// RegionFluxUi.h
// The distance an HI mass of a region is computed at (issue #401), shared by
// the cube and image viewers and kept between sessions, and the dialog that
// sets it (Tools ▸ Regions ▸ HI Mass Distance…).

#include "RegionFlux.h"

#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QSettings>
#include <QString>
#include <QWidget>

namespace visivo::flux {

inline QString hiDistanceSettingsKey()
{
    return QStringLiteral("regionFlux/hiDistance");
}

/// The distance set in the dialog; not ok when none is.
inline TypedDistance storedHiDistance()
{
    return parseDistance(QSettings().value(hiDistanceSettingsKey()).toString());
}

/// Ask for the distance ("25", "25 Mpc", "z=0.0046"); an empty answer clears
/// it. False when cancelled or not understood.
inline bool askHiDistance(QWidget *parent)
{
    const QString current = QSettings().value(hiDistanceSettingsKey()).toString();
    bool ok = false;
    const QString text = QInputDialog::getText(
            parent, QStringLiteral("HI Mass Distance"),
            QStringLiteral("Distance for the HI mass of a region, M_HI = 2.356\u00d710\u2075 D\u00b2 S:\n"
                           "in Mpc (25), or a redshift (z=0.0046, luminosity distance in "
                           "Planck18).\nLeave empty to show no mass."),
            QLineEdit::Normal, current, &ok);
    if (!ok)
        return false;
    if (text.trimmed().isEmpty()) {
        QSettings().remove(hiDistanceSettingsKey());
        return true;
    }
    if (!parseDistance(text).ok) {
        QMessageBox::warning(parent, QStringLiteral("HI Mass Distance"),
                             QStringLiteral("\"%1\" is not a distance: type Mpc (25) or a "
                                            "redshift (z=0.0046).")
                                     .arg(text));
        return false;
    }
    QSettings().setValue(hiDistanceSettingsKey(), text.trimmed());
    return true;
}

} // namespace visivo::flux

#endif // REGION_FLUX_UI_H
