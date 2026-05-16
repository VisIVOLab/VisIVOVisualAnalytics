#ifndef DiagnosticsWindow_h
#define DiagnosticsWindow_h

#include <QSortFilterProxyModel>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QPushButton;
class QTableView;

class DiagnosticsModel;
struct DiagnosticEntry;

class DiagnosticsFilterModel : public QSortFilterProxyModel
{
    Q_OBJECT

public:
    explicit DiagnosticsFilterModel(QObject *parent = nullptr);

    void setLevelFilter(const QString &level);
    void setCategoryFilter(const QString &category);

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const override;

private:
    QString m_levelFilter;
    QString m_categoryFilter;
};

class DiagnosticsWindow : public QWidget
{
    Q_OBJECT

public:
    explicit DiagnosticsWindow(QWidget *parent = nullptr, Qt::WindowFlags flags = Qt::Window);

    /// Show the global diagnostics window. Lazily constructs a process-wide
    /// singleton so any window (MainWindow, vtkWindowCube, vtkWindowImage,
    /// …) can call this without owning its own instance. Returns the
    /// singleton pointer for callers that want to position it.
    static DiagnosticsWindow *showSingleton();

private slots:
    void copySelectedMessages();

private:
    DiagnosticsModel *m_model;
    DiagnosticsFilterModel *m_filterModel;
    QTableView *m_tableView;
    QComboBox *m_levelCombo;
    QComboBox *m_categoryCombo;
    QCheckBox *m_autoScrollCheck;
    QPushButton *m_copyButton;
    QPushButton *m_clearButton;
};

#endif
