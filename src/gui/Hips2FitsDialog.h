#ifndef Hips2FitsDialog_h
#define Hips2FitsDialog_h

// Hips2FitsDialog.h
// Cut a FITS out of any HiPS survey (CDS hips2fits).
//
// Ported from the legacy client's HiPS2FITSForm (ViaLactea Visual Analytics
// 1.7.4). Three things moved: the request is built and validated on the backend
// (app/hips2fits.py), so a blank field is refused with a sentence instead of a
// 400 from CDS; the survey list comes from the backend too, rather than being a
// QStringList compiled into the dialog; and the result is written into the
// Workspace, so it is a file the rest of the application can open — the legacy
// saved it to a temp directory and offered to open it in a window of its own.

#include "BackendClient.h"

#include <QDialog>
#include <QFutureWatcher>
#include <QPointer>

class QComboBox;
class QDialogButtonBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QSpinBox;

class Hips2FitsDialog : public QDialog
{
    Q_OBJECT

public:
    Hips2FitsDialog(const QString &backendUrl, const QString &backendToken,
                    QWidget *parent = nullptr);
    ~Hips2FitsDialog() override;

    /// Start the form at a position — used when the request comes from a viewer
    /// that already knows where the user is looking.
    void setPosition(double ra, double dec, double fovDeg);

signals:
    /// The cutout, in the workspace. FITS only: a jpg/png is not something the
    /// viewers open, and the dialog says where it was saved instead.
    void cutoutReady(const QString &backendPath);

private slots:
    void onOptionsFinished();
    void onCutoutFinished();
    void updateFormatDependentFields();

private:
    void buildUi();
    void accept() override;

    QString m_backendUrl;
    QString m_backendToken;

    QFutureWatcher<BackendHips2FitsOptions> m_optionsWatcher;
    QFutureWatcher<BackendHips2FitsResult> m_cutoutWatcher;

    QPointer<QComboBox> m_survey;
    QPointer<QSpinBox> m_width;
    QPointer<QSpinBox> m_height;
    QPointer<QDoubleSpinBox> m_ra;
    QPointer<QDoubleSpinBox> m_dec;
    QPointer<QDoubleSpinBox> m_fov;
    QPointer<QDoubleSpinBox> m_rotation;
    QPointer<QComboBox> m_projection;
    QPointer<QComboBox> m_coordsys;
    QPointer<QComboBox> m_format;
    QPointer<QDoubleSpinBox> m_minCut;
    QPointer<QDoubleSpinBox> m_maxCut;
    QPointer<QComboBox> m_stretch;
    QPointer<QLineEdit> m_outputName;
    QPointer<QLabel> m_status;
    QPointer<QDialogButtonBox> m_buttons;
};

#endif   // Hips2FitsDialog_h
