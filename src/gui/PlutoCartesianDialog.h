#ifndef PlutoCartesianDialog_h
#define PlutoCartesianDialog_h

// PlutoCartesianDialog.h
// The Cartesian view of a PLUTO output on polar, cylindrical, spherical or
// stretched cells (issue #527): resolution, mirroring, which variables, and
// the cost before the job. The resampling runs on the backend
// (/v1/simulations/pluto/cartesian); the cube it writes opens like any volume.

#include "BackendClient.h"

#include <QDialog>
#include <QFutureWatcher>
#include <QPointer>
#include <QString>

class QCheckBox;
class QLabel;
class QProgressDialog;
class QPushButton;
class QSpinBox;

class PlutoCartesianDialog : public QDialog
{
    Q_OBJECT

public:
    PlutoCartesianDialog(const QString &plutoPath, const QString &backendUrl,
                         const QString &backendToken, QWidget *parent = nullptr);
    ~PlutoCartesianDialog() override;

signals:
    /// A cube was written on the backend and the user asked to open it.
    void cubeReady(const QString &backendPath);

private slots:
    void onPlanFinished();
    void onExtractFinished();
    void refreshPlan();
    void extract();

private:
    void setBusy(bool busy, const QString &message = QString());
    void fitToContents();

    QString m_path;
    QString m_backendUrl;
    QString m_backendToken;
    /// A setting changed while a /plan request was in flight: its answer is
    /// for settings the user has moved on from.
    bool m_planDirty{ false };

    QFutureWatcher<visivo::plutoview::Plan> m_planWatcher;
    QFutureWatcher<visivo::plutoview::Result> m_extractWatcher;
    QPointer<QProgressDialog> m_progressSheet;

    QPointer<QLabel> m_planLabel;
    QPointer<QLabel> m_statusLabel;
    QPointer<QSpinBox> m_millions;
    QPointer<QCheckBox> m_mirror;
    QPointer<QCheckBox> m_all;
    QPointer<QPushButton> m_extractButton;
};

#endif   // PlutoCartesianDialog_h
