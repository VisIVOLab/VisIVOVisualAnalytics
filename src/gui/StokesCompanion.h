#pragma once
// StokesCompanion.h
// Naming rules for finding the Stokes Q/U/V files that belong with a Stokes I
// image. Pure string work, deliberately kept out of the viewer: the viewer only
// decides WHERE to look (a local directory, or the backend's filesystem when the
// backend is remote), while the candidate names and the preference order between
// them are pinned by tests here.
//
// The caller supplies the set of file names that actually exist in the
// directory, so the same rules work against a local listing and against a
// backend listing without a second copy of the patterns.

#include <QSet>
#include <QString>
#include <QStringList>

namespace visivo {

/// Candidate file names for `role` ("Q", "U" or "V") derived from a Stokes I
/// file name, most-specific first. Never contains `fileName` itself.
inline QStringList stokesCompanionCandidates(const QString &fileName, const QString &role)
{
    if (fileName.isEmpty() || role.isEmpty())
        return {};

    const QString lower = role.toLower();
    QStringList candidates;
    // Explicit "Stokes<letter>" in the name, in the three casings seen in the wild.
    candidates << QString(fileName).replace(QStringLiteral("StokesI"),
                                            QStringLiteral("Stokes") + role);
    candidates << QString(fileName).replace(QStringLiteral("stokesI"),
                                            QStringLiteral("stokes") + lower);
    candidates << QString(fileName).replace(QStringLiteral("STOKESI"),
                                            QStringLiteral("STOKES") + role);
    // ASKAP/MeerKAT products name the component with a bare suffix instead.
    candidates << QString(fileName).replace(QStringLiteral("_I.fits"),
                                            QStringLiteral("_") + role + QStringLiteral(".fits"));
    candidates << QString(fileName).replace(QStringLiteral("_I_PB.fits"),
                                            QStringLiteral("_") + role + QStringLiteral("_PB.fits"));
    candidates << QString(fileName).replace(QStringLiteral("_I.fits.gz"),
                                            QStringLiteral("_") + role + QStringLiteral(".fits.gz"));
    candidates << QString(fileName).replace(QStringLiteral("_I_PB.fits.gz"),
                                            QStringLiteral("_") + role
                                                    + QStringLiteral("_PB.fits.gz"));
    // A primary-beam corrected intensity image next to UNCORRECTED companions.
    // MGCLS is published this way: of its 52 fields, 32 ship FIELD_I_PB.fits.gz
    // while Q, U and V have no _PB variant at all — so the rules above, which
    // only ever look for _Q_PB, find nothing for those 32 fields and the whole
    // set has to be located by hand. Less specific than the _PB → _PB rules, so
    // it is appended after them and only wins when they match no file.
    candidates << QString(fileName).replace(QStringLiteral("_I_PB.fits.gz"),
                                            QStringLiteral("_") + role
                                                    + QStringLiteral(".fits.gz"));
    candidates << QString(fileName).replace(QStringLiteral("_I_PB.fits"),
                                            QStringLiteral("_") + role
                                                    + QStringLiteral(".fits"));
    candidates.removeDuplicates();
    candidates.removeAll(fileName);   // a pattern that matched nothing is not a candidate
    return candidates;
}

/// The first candidate name for `role` that exists in `namesInDirectory`, or an
/// empty string when the companion is not there.
inline QString pickStokesCompanion(const QString &fileName, const QString &role,
                                   const QSet<QString> &namesInDirectory)
{
    const QStringList candidates = stokesCompanionCandidates(fileName, role);
    for (const QString &candidate : candidates) {
        if (namesInDirectory.contains(candidate))
            return candidate;
    }
    return QString();
}

}   // namespace visivo
