#ifndef AmrUniformDialog_h
#define AmrUniformDialog_h

// AmrUniformDialog.h
// The uniform cube of an AMR output (issue #550): which field, at which
// refinement level, over the whole box or a region of it, with the cost shown
// before the job. The resampling runs on the backend
// (/v1/simulations/amr/uniform); the cube it writes opens like any volume.
// Built like PlutoCartesianDialog, whose plan / job handling it follows.

#include "BackendClient.h"

#include <QDialog>
#include <QFutureWatcher>
#include <QPointer>
#include <QString>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QProgressDialog;
class QPushButton;
class QSpinBox;

class AmrUniformDialog : public QDialog
{
    Q_OBJECT

public:
    AmrUniformDialog(const QString &outputPath, const QString &backendUrl,
                     const QString &backendToken, QWidget *parent = nullptr);
    ~AmrUniformDialog() override;

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
    /// The region the spin boxes describe (unset while the box is off).
    visivo::amrview::Region region() const;
    /// Fill the field and level lists from the first plan, once.
    void populateChoices(const visivo::amrview::Plan &plan);

    QString m_path;
    QString m_backendUrl;
    QString m_backendToken;
    /// A setting changed while a /plan request was in flight: its answer is
    /// for settings the user has moved on from.
    bool m_planDirty{ false };
    bool m_populated{ false };
    QString m_firstField{ QStringLiteral("density") };   ///< the field of the first plan

    QFutureWatcher<visivo::amrview::Plan> m_planWatcher;
    QFutureWatcher<visivo::amrview::Result> m_extractWatcher;
    QPointer<QProgressDialog> m_progressSheet;

    QPointer<QLabel> m_planLabel;
    QPointer<QLabel> m_statusLabel;
    QPointer<QComboBox> m_field;
    QPointer<QComboBox> m_level;
    QPointer<QSpinBox> m_millions;
    QPointer<QCheckBox> m_regionOn;
    QPointer<QDoubleSpinBox> m_lo[3];
    QPointer<QDoubleSpinBox> m_hi[3];
    QPointer<QPushButton> m_extractButton;
};

#endif   // AmrUniformDialog_h
