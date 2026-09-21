#ifndef VlkbQueryComposer_h
#define VlkbQueryComposer_h

// VlkbQueryComposer.h
// The VLKB catalogue queries, over a Galactic box or in ADQL.
//
// Ported from the legacy client's VLKBSimpleQueryComposer (ViaLactea Visual
// Analytics 1.7.4). The queries live on the backend now (app/vlkb_tap.py) —
// where they can be corrected without releasing a client, and where the one
// that was missing (bubbles: the legacy had the mode but no query) has been
// written. The result is saved as a CSV in the Workspace, which is what lets
// "run a query" end in a catalogue the viewers open rather than in a .dat file.
//
// The box can be prefilled from the image the user is looking at, which is what
// the legacy's rubber-band selector did.

#include "BackendClient.h"

#include <QDialog>
#include <QFutureWatcher>
#include <QPointer>

class QComboBox;
class QDialogButtonBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTableWidget;

class VlkbQueryComposer : public QDialog
{
    Q_OBJECT

public:
    VlkbQueryComposer(const QString &backendUrl, const QString &backendToken,
                      QWidget *parent = nullptr);
    ~VlkbQueryComposer() override;

    /// Start from the footprint of the image the user is looking at.
    void setBox(double glonMin, double glonMax, double glatMin, double glatMax);

signals:
    /// The saved result, ready to open in the 3-D catalogue viewer.
    void catalogueReady(const QString &backendPath);

private slots:
    void onPresetsFinished();
    void onQueryFinished();
    void updatePresetFields();
    void runQuery();

private:
    void buildUi();

    QString m_backendUrl;
    QString m_backendToken;
    QString m_savedPath;

    QFutureWatcher<BackendVlkbPresetsResult> m_presetsWatcher;
    QFutureWatcher<BackendVlkbTapResult> m_queryWatcher;

    QPointer<QComboBox> m_preset;
    QPointer<QComboBox> m_band;
    QPointer<QDoubleSpinBox> m_glonMin;
    QPointer<QDoubleSpinBox> m_glonMax;
    QPointer<QDoubleSpinBox> m_glatMin;
    QPointer<QDoubleSpinBox> m_glatMax;
    QPointer<QSpinBox> m_maxRows;
    QPointer<QPlainTextEdit> m_adql;
    QPointer<QLineEdit> m_outputName;
    QPointer<QTableWidget> m_table;
    QPointer<QLabel> m_status;
    QPointer<QDialogButtonBox> m_buttons;
    QPointer<QPushButton> m_openButton;
};

#endif   // VlkbQueryComposer_h
