#ifndef BackendsSettingsWidget_h
#define BackendsSettingsWidget_h

#include "../Settings.h"

#include <QPointer>
#include <QWidget>

class QPushButton;
class QTableWidget;

/// Manages the list of remote VisIVO backend nodes (one per SKA Regional
/// Centre / data centre) shown in the Settings dialog. The local backend
/// is auto-managed by Settings — only non-local nodes appear in the
/// table here. Editing happens in-memory; the SettingsDialog calls
/// ``saveTo()`` on accept to persist.
class BackendsSettingsWidget : public QWidget
{
    Q_OBJECT
public:
    explicit BackendsSettingsWidget(Settings *settings, QWidget *parent = nullptr);

    /// Persist the current table state into the given Settings instance.
    /// Called from SettingsDialog::accept().
    void saveTo(Settings *settings);

private slots:
    void addNode();
    void editSelected();
    void removeSelected();
    void onSelectionChanged();

private:
    void rebuildTable();
    void appendRow(const BackendNode &node);

    QPointer<Settings> m_settings;
    QTableWidget *m_table{ nullptr };
    QPushButton *m_addBtn{ nullptr };
    QPushButton *m_editBtn{ nullptr };
    QPushButton *m_removeBtn{ nullptr };

    /// In-memory working copy of the non-local nodes. Mutated by the
    /// add/edit/remove slots and flushed to Settings in saveTo().
    QList<BackendNode> m_nodes;
};

#endif  // BackendsSettingsWidget_h
