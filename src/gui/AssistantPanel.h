#ifndef AssistantPanel_h
#define AssistantPanel_h

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QStringList>
#include <QWidget>

class QLineEdit;
class QPushButton;
class QTextBrowser;
class QLabel;
class QVBoxLayout;
class QNetworkAccessManager;
class QNetworkReply;
struct BackendAssistantResult;

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

    /// Example starter questions shown as clickable chips while the conversation
    /// is empty (discoverability). Each viewer passes prompts tailored to its
    /// data; a generic default is used until then. Clicking a chip sends it.
    void setSuggestions(const QStringList &prompts);

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
    void onSettingsUpdated();   ///< re-apply after the global Settings dialog changes it
    void onClearConversation();   ///< start a fresh conversation (drops follow-up context)
    /// Show or hide the "cannot answer" banner from what is known of m_url.
    void refreshProblemBanner();

protected:
    void changeEvent(QEvent *event) override;

private:
    void applyStatusColour();   ///< the status line's readable, subdued colour
    /// Push the persisted copilot provider config (Settings) to the backend, so
    /// the user configures the provider once and it auto-applies on every connect.
    void applyProviderFromSettings();
    /// Record what the backend at @p url said stops the copilot ("" = nothing),
    /// so this panel, the other viewers' panels and Settings all show it.
    /// @p ticket is CopilotStatus's number for the request this answers.
    void reportProblem(const QString &url, const QString &problem, quint64 ticket);
    /// Ask the backend again after an answer: a self-hosted server with no or
    /// several models is only found out at the first question.
    void refreshStatusAfterAnswer();
    /// Run a deferred provider change once no query and no stream is live.
    void maybeApplyPendingProvider();
    /// Abort and detach a stream reply that is still open.
    void closeLiveStream();
    void renderConversation();   ///< rebuild the transcript HTML from m_turns/logs
    void appendError(const QString &text);
    void setBusy(bool busy);
    /// Stream a query over Server-Sent-Events so the UI can show which tool is
    /// running as the agent works. Falls through to the same result handling as
    /// the blocking path once the final ``result`` event arrives.
    void startStream(const QString &question);
    void sendStreamRequest(const QString &question);   ///< the actual SSE POST
    void handleStreamBuffer(const QString &question);   ///< parse complete SSE blocks
    void applyResult(const BackendAssistantResult &r, const QString &question);
    void stopStream();   ///< user-cancel the in-flight query (aborts the SSE reply)
    void refreshSendButton();   ///< Send ↔ Stop depending on whether a stream is live
    /// One rendered transcript entry (a chat bubble or an inline system note).
    struct Entry
    {
        enum Kind { User, Assistant, Error, ActionLog } kind{ User };
        QString text;          ///< the message / answer / error text
        QString provider;      ///< assistant only: provider label
        bool configured{ true };
        /// A specific reason was known for an unconfigured provider, so the
        /// generic "no provider" hint would only repeat it less precisely.
        bool problemKnown{ false };
        bool grounded{ false };
        QString repro;         ///< assistant only: reproducibility script
        QString provenance;    ///< assistant only: tool-call provenance HTML
        QString actionLog;     ///< ActionLog only: the applied-directives HTML
    };

    QString m_url;
    QString m_token;
    QString m_datasetId;
    bool m_busy{ false };
    /// A Settings change landed while the panel was busy; applied on release.
    bool m_providerApplyPending{ false };
    bool m_providerAutoApplied{ false };   ///< auto-apply the saved config only once
    /// Bumped per status request, provider POST and backend change; a status
    /// reply from an older generation is dropped.
    quint64 m_statusGeneration{ 0 };
    /// The provider the backend last reported as active, for the banner, and
    /// whether it is the server's own configuration rather than the user's.
    QString m_activeName;
    bool m_activeFromServer{ true };

    QList<Entry> m_entries;    ///< everything shown in the transcript, for re-render
    QJsonArray m_turns;        ///< prior user/assistant TEXT turns sent for follow-up context
    QStringList m_suggestionPrompts;   ///< current starter-question chips
    void rebuildSuggestions();         ///< (re)create the chip buttons

    QTextBrowser *m_transcript{ nullptr };
    QLineEdit *m_input{ nullptr };
    QPushButton *m_send{ nullptr };
    QPushButton *m_clear{ nullptr };
    QLabel *m_status{ nullptr };
    QLabel *m_problem{ nullptr };   ///< why the copilot cannot answer (hidden if nothing is known)
    QWidget *m_suggestions{ nullptr };        ///< chip container (shown when empty)
    QVBoxLayout *m_suggestionsLayout{ nullptr };

    // ── Streaming (Server-Sent-Events) ────────────────────────────────────────
    QNetworkAccessManager *m_net{ nullptr };
    QNetworkReply *m_streamReply{ nullptr };
    QByteArray m_streamBuf;          ///< accumulated SSE bytes, parsed block-by-block
    bool m_streamGotResult{ false }; ///< a final result/error event arrived
    bool m_userStopped{ false };     ///< the user cancelled this stream (no error banner)
    QString m_scopeDatasetId;        ///< scope captured at send, to guard stale replies
    QString m_scopeUrl;
    QString m_scopeToken;
};

#endif  // AssistantPanel_h
