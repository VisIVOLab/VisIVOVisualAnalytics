#ifndef ArepoExtractDialog_h
#define ArepoExtractDialog_h

// ArepoExtractDialog.h
// Browse an AREPO snapshot and extract a window of it as a uniform cube.
//
// AREPO is a moving-mesh code: the snapshot is an unstructured set of cells,
// so there is nothing to display until a window of it has been resampled onto a
// grid. That resampling is yt's, and it runs on the **backend**
// (/v1/simulations/arepo/*), which is where the simulation file lives and where
// Python is known to work. The legacy client did it by spawning
// `python arepo-extractor.py` against an interpreter path kept in QSettings.
//
// The dialog shows the cost of the job before it is started: the grid, the cell
// size and the memory come from /plan, which needs no yt and no extraction.

#include "BackendClient.h"

#include <QDialog>
#include <QFutureWatcher>
#include <QPointer>
#include <QString>

class QCheckBox;
class QDialogButtonBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QProgressDialog;
class QPushButton;
class QSpinBox;
class QTreeWidget;
class QTreeWidgetItem;

class ArepoExtractDialog : public QDialog
{
    Q_OBJECT

public:
    ArepoExtractDialog(const QString &snapshotPath, const QString &backendUrl,
                       const QString &backendToken, QWidget *parent = nullptr);
    ~ArepoExtractDialog() override;

signals:
    /// A cube was written on the backend and the user asked to open it.
    void cubeReady(const QString &backendPath);

private slots:
    void onInspectFinished();
    void onPlanFinished();
    void onExtractFinished();
    void refreshPlan();
    void extract();

private:
    void buildUi();
    void populateTree(const BackendArepoNode &node, QTreeWidgetItem *parent);
    void applyInspection(const BackendArepoInspectResult &result);
    QVector<double> windowKpc() const;
    QVector<double> offsetKpc() const;
    void setBusy(bool busy, const QString &message = QString());

    QString m_path;
    QString m_backendUrl;
    QString m_backendToken;
    bool m_ytAvailable{ true };
    /// A parameter changed while a /plan request was in flight. Dropping the
    /// update and never asking again left the panel quoting a grid for
    /// settings the user had already moved on from — and Extract enabled on
    /// the strength of it.
    bool m_planDirty{ false };

    QFutureWatcher<BackendArepoInspectResult> m_inspectWatcher;
    QFutureWatcher<BackendArepoPlan> m_planWatcher;
    QFutureWatcher<BackendArepoExtractResult> m_extractWatcher;
    QPointer<QProgressDialog> m_progressSheet;

    QPointer<QTreeWidget> m_tree;
    QPointer<QLabel> m_boxSizeLabel;
    QPointer<QLabel> m_planLabel;
    QPointer<QLabel> m_statusLabel;
    QPointer<QSpinBox> m_level;
    QPointer<QLineEdit> m_particle;
    QPointer<QLineEdit> m_fields;
    QPointer<QDoubleSpinBox> m_sizeX, m_sizeY, m_sizeZ;
    QPointer<QDoubleSpinBox> m_offsetX, m_offsetY, m_offsetZ;
    QPointer<QPushButton> m_extractButton;
};

#endif   // ArepoExtractDialog_h
