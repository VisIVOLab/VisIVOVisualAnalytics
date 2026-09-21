#ifndef FilterFitsDialog_h
#define FilterFitsDialog_h

// FilterFitsDialog.h
// Smooth and/or shrink the image in front of the user, into a new file.
//
// Ported from the legacy client's FilterFITSDialog (ViaLactea Visual Analytics
// 1.7.4). The arithmetic moved to the backend (app/compute/imutils.py), which
// is where the three corrections live: blanks no longer eat their
// neighbourhood, the beam grows with the kernel, and a per-pixel unit is summed
// rather than averaged. What stayed here is the shape of the dialog — two
// independent actions on one file — plus what the legacy could not show,
// because it never computed it: what each choice does in arcseconds, and to
// how many pixels.

#include "BackendClient.h"

#include <QDialog>
#include <QFutureWatcher>
#include <QPointer>
#include <QString>

class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QSpinBox;

class FilterFitsDialog : public QDialog
{
    Q_OBJECT

public:
    /// @param path       the FITS to filter, on the BACKEND's filesystem
    /// @param pixelArcsec the image's pixel scale, for the live preview of what
    ///                   a FWHM in pixels means on the sky (0 = unknown)
    FilterFitsDialog(const QString &path, double pixelArcsec, int width, int height,
                     const QString &backendUrl, const QString &backendToken,
                     QWidget *parent = nullptr);
    ~FilterFitsDialog() override;

signals:
    /// The filtered file, ready to open. The caller decides what that means:
    /// the image viewer adds it as a layer, the cube viewer opens a window.
    void filtered(const QString &backendPath);

private slots:
    void onFinished();
    void updateSummary();

private:
    void buildUi();
    void accept() override;

    QString m_path;
    double m_pixelArcsec{ 0. };
    int m_width{ 0 };
    int m_height{ 0 };
    QString m_backendUrl;
    QString m_backendToken;

    QFutureWatcher<BackendFilterFitsResult> m_watcher;

    QPointer<QCheckBox> m_smoothBox;
    QPointer<QDoubleSpinBox> m_fwhm;
    QPointer<QCheckBox> m_regridBox;
    QPointer<QSpinBox> m_factor;
    QPointer<QComboBox> m_combine;
    QPointer<QLineEdit> m_outputName;
    QPointer<QLabel> m_summary;
    QPointer<QLabel> m_status;
    QPointer<QDialogButtonBox> m_buttons;
};

#endif   // FilterFitsDialog_h
