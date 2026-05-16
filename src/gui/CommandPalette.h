#ifndef CommandPalette_h
#define CommandPalette_h

#include <QDialog>
#include <QPointer>
#include <QString>

#include <functional>
#include <vector>

class QKeyEvent;
class QLineEdit;
class QListWidget;
class QListWidgetItem;

// ---------------------------------------------------------------------------
// Layout B "command palette" — fullscreen-modal-feel overlay invoked with ⌘K.
// Hosts a search field and a grouped list of actions; selecting one invokes
// the registered std::function and closes the palette.
//
// Usage:
//   auto *palette = new CommandPalette(this);
//   palette->addAction({.title = "Open Remote Dataset…",
//                       .subtitle = "File · backend filesystem",
//                       .shortcut = "⌘O",
//                       .group = "File",
//                       .trigger = [this]() { this->openData(); }});
//   palette->open();   // shows centered on parent
// ---------------------------------------------------------------------------
class CommandPalette : public QDialog
{
    Q_OBJECT

public:
    struct Action
    {
        QString glyph;           // small unicode icon (◯ ▦ ⌘ ∿ …) — optional
        QString title;           // "Compute Moment Map…"
        QString subtitle;        // "Tools · M0–M10 · channel range"
        QString shortcut;        // "⇧⌘M" — visual hint only, not a real binding
        QString group;           // "Suggested", "File", "Recent datasets" …
        std::function<void()> trigger;
    };

    explicit CommandPalette(QWidget *parent = nullptr);

    // Registers an action. Order is preserved within each group; groups are
    // rendered in the order their first action was added.
    void addAction(Action action);

    // Removes every action whose group matches the given name (case-sensitive).
    // Useful for refreshing dynamic groups like "Recent datasets" before show.
    void clearGroup(const QString &group);

protected:
    void showEvent(QShowEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private slots:
    void onQueryChanged(const QString &text);
    void onItemActivated(QListWidgetItem *item);

private:
    void rebuildList();
    int  selectableRowCount() const;
    void selectFirstSelectable();
    void moveSelection(int delta);

    QPointer<QLineEdit>   m_input;
    QPointer<QListWidget> m_list;
    std::vector<Action>   m_actions;
    QString               m_filter;
};

#endif // CommandPalette_h
