#ifndef AssistantPanel_h
#define AssistantPanel_h

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QWidget>

class QLineEdit;
class QPushButton;
class QTextBrowser;
class QLabel;

/// Scientific copilot chat panel (R-22).
///
/// A thin front-end over BackendClient::assistantQuery. The copilot is a
/// provenance-bound, tool-calling orchestrator on the backend: every number in
/// an answer comes from a recorded tool call, and each answer carries a
/// reproducibility script. This panel just renders the conversation and the
/// per-answer provenance footer; it holds no scientific logic of its own.
///
/// When no LLM provider is configured server-side the backend still answers
/// (explaining how to enable one), so the panel degrades gracefully.
class AssistantPanel : public QWidget
{
    Q_OBJECT
public:
    explicit AssistantPanel(QWidget *parent = nullptr);

    /// Backend endpoint used for queries. A synchronous BackendClient is built
    /// per query inside a worker thread (matching the rest of the app), so no
    /// long-lived client pointer is held.
    void setBackend(const QString &url, const QString &token);
    /// The dataset the questions are about (may be empty).
    void setDatasetId(const QString &datasetId);

signals:
    /// Emitted when a query starts / finishes so the host can show busy state.
    void busyChanged(bool busy);

    /// The copilot asked to change the visualisation. The host viewer applies the
    /// directives (each a {"type":…, …} object) in order; they are reversible.
    void actionsRequested(const QList<QJsonObject> &actions);

    /// The user clicked "Undo" on an applied-actions log line; the host viewer
    /// reverts the last copilot visualisation change.
    void undoRequested();

private slots:
    void onSend();
    void onSettings();   ///< choose the LLM provider (Claude / OpenAI / own server)
    void onSettingsUpdated();   ///< re-apply after the global Settings dialog changes it
    void onClearConversation();   ///< start a fresh conversation (drops follow-up context)

private:
    /// Push the persisted copilot provider config (Settings) to the backend, so
    /// the user configures the provider once and it auto-applies on every connect.
    void applyProviderFromSettings();
    void renderConversation();   ///< rebuild the transcript HTML from m_turns/logs
    void appendError(const QString &text);
    void setBusy(bool busy);
    /// One rendered transcript entry (a chat bubble or an inline system note).
    struct Entry
    {
        enum Kind { User, Assistant, Error, ActionLog } kind{ User };
        QString text;          ///< the message / answer / error text
        QString provider;      ///< assistant only: provider label
        bool configured{ true };
        bool grounded{ false };
        QString repro;         ///< assistant only: reproducibility script
        QString provenance;    ///< assistant only: tool-call provenance HTML
        QString actionLog;     ///< ActionLog only: the applied-directives HTML
    };

    QString m_url;
    QString m_token;
    QString m_datasetId;
    bool m_busy{ false };
    bool m_providerAutoApplied{ false };   ///< auto-apply the saved config only once

    QList<Entry> m_entries;    ///< everything shown in the transcript, for re-render
    QJsonArray m_turns;        ///< prior user/assistant TEXT turns sent for follow-up context

    QTextBrowser *m_transcript{ nullptr };
    QLineEdit *m_input{ nullptr };
    QPushButton *m_send{ nullptr };
    QPushButton *m_settings{ nullptr };
    QPushButton *m_clear{ nullptr };
    QLabel *m_status{ nullptr };
};

#endif  // AssistantPanel_h
