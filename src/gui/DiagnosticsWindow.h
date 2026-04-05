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
