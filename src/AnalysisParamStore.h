#ifndef AnalysisParamStore_h
#define AnalysisParamStore_h

#include <QString>
#include <QVariantMap>

/// Per-dataset persistence of analysis-tool parameters (noise region, kinematic
/// tilted-ring model, …) so a tool dialog re-opens pre-filled with the values
/// the user last used *for that dataset*. Keyed by a stable dataset key (the
/// FITS file path — NOT the session-scoped backend dataset id, which changes
/// each open). Values are written into the same ``Settings.ini`` the rest of the
/// app uses, under an ``AnalysisParams`` group.
///
/// Pure Qt (QSettings) — no GUI/VTK — so it is unit testable with a temp ini.
class AnalysisParamStore
{
public:
    /// @param iniPath the settings file to read/write; defaults to the shared
    /// application Settings.ini. Tests pass a temporary path.
    explicit AnalysisParamStore(QString iniPath = defaultIniPath());

    /// Persist @p params for (@p datasetKey, @p group). Existing values in that
    /// group are overwritten; other groups/datasets are untouched.
    void save(const QString &datasetKey, const QString &group, const QVariantMap &params) const;

    /// Load the params previously saved for (@p datasetKey, @p group). Returns an
    /// empty map when nothing was stored.
    QVariantMap load(const QString &datasetKey, const QString &group) const;

    /// The shared application Settings.ini path (~/<AppName>/Settings.ini).
    static QString defaultIniPath();

private:
    /// Collapse an arbitrary dataset key (e.g. a file path) into a stable,
    /// separator-free QSettings token.
    static QString datasetToken(const QString &datasetKey);

    QString m_iniPath;
};

#endif  // AnalysisParamStore_h
