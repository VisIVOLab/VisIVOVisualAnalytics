#include "AssistantPanel.h"

#include "CopilotActionRouter.h"
#include "Settings.h"
#include "app/BackendClient.h"
#include "app/BackendClientFactory.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QFuture>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QJsonDocument>
#include <QLineEdit>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPalette>
#include <QPushButton>
#include <QTimer>
#include <QScrollBar>
#include <QTextBrowser>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrent>

using namespace Qt::StringLiterals;

namespace {
QString esc(const QString &s)
{
    return s.toHtmlEscaped();
}

/// Chat-bubble colour palette, chosen per light/dark theme. QTextDocument does
/// not resolve `palette(...)`, so we pick concrete colours from the widget's
/// actual palette lightness.
struct BubbleTheme
{
    QString userBg, userFg, botBg, botFg, meta, rule, errBg, errFg;
};
BubbleTheme bubbleTheme(const QPalette &pal)
{
    const bool dark = pal.color(QPalette::Base).lightness() < 128;
    if (dark) {
        return { u"#2b4a6f"_s, u"#eaf1fb"_s, u"#2a2d31"_s, u"#e6e6e6"_s,
                 u"#9aa0a6"_s, u"#3a3d42"_s, u"#4a2626"_s, u"#f3b9b9"_s };
    }
    return { u"#dbe8fb"_s, u"#173a63"_s, u"#f1f2f4"_s, u"#1b1c1d"_s,
             u"#6b7075"_s, u"#e2e4e8"_s, u"#fbe3e3"_s, u"#8a1f1f"_s };
}
}  // namespace

AssistantPanel::AssistantPanel(QWidget *parent) : QWidget(parent)
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(6);

    auto *titleRow = new QHBoxLayout();
    auto *title = new QLabel(u"Scientific copilot"_s, this);
    title->setStyleSheet(u"font-weight: 600;"_s);
    m_clear = new QPushButton(u"＋ New chat"_s, this);
    m_clear->setToolTip(u"Start a fresh conversation (clears the follow-up context)"_s);
    m_clear->setFlat(true);
    m_clear->setCursor(Qt::PointingHandCursor);
    m_clear->setStyleSheet(u"QPushButton { color: palette(mid); border: none; padding: 2px 6px; }"
                           u"QPushButton:hover { color: palette(text); }"_s);
    titleRow->addWidget(title);
    titleRow->addStretch(1);
    titleRow->addWidget(m_clear);
    root->addLayout(titleRow);

    m_transcript = new QTextBrowser(this);
    m_transcript->setOpenExternalLinks(false);
    m_transcript->setOpenLinks(false);   // handle the "Undo" anchor ourselves
    m_transcript->setFrameShape(QFrame::NoFrame);
    connect(m_transcript, &QTextBrowser::anchorClicked, this, [this](const QUrl &u) {
        const QString s = u.toString();
        if (s == u"copilot:undo"_s) {
            // Undo the last copilot change across whichever view(s) it touched.
            CopilotActionRouter::instance()->undoLast();
        } else if (s.startsWith(u"copilot:copy:"_s) || s.startsWith(u"copilot:copyrepro:"_s)) {
            const bool repro = s.startsWith(u"copilot:copyrepro:"_s);
            bool ok = false;
            const int idx = s.section(u":"_s, -1).toInt(&ok);
            if (ok && idx >= 0 && idx < m_entries.size()) {
                const Entry &e = m_entries.at(idx);
                const QString text = repro ? e.repro : e.text;
                if (!text.isEmpty()) {
                    QGuiApplication::clipboard()->setText(text);
                    m_status->setText(repro ? u"Reproducibility script copied."_s
                                            : u"Answer copied to clipboard."_s);
                }
            }
        }
    });
    m_transcript->setPlaceholderText(
            u"Ask about the loaded dataset - units, statistics, a moment map, a "
            u"distance. Every number in the answer is backed by a tool call.\n\n"
            u"Ask follow-up questions freely (the copilot remembers the conversation)."_s);
    root->addWidget(m_transcript, 1);

    // Starter-question chips, shown only while the conversation is empty.
    m_suggestions = new QWidget(this);
    m_suggestionsLayout = new QVBoxLayout(m_suggestions);
    m_suggestionsLayout->setContentsMargins(2, 0, 2, 0);
    m_suggestionsLayout->setSpacing(4);
    root->addWidget(m_suggestions);

    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    m_status->setStyleSheet(u"color: palette(mid);"_s);
    root->addWidget(m_status);

    auto *row = new QHBoxLayout();
    m_input = new QLineEdit(this);
    m_input->setPlaceholderText(u"Ask a question…"_s);
    m_input->setClearButtonEnabled(true);
    m_send = new QPushButton(u"Send"_s, this);
    m_send->setDefault(true);
    row->addWidget(m_input, 1);
    row->addWidget(m_send);
    root->addLayout(row);

    // While a query STREAMS the Send button becomes a Stop button. Other busy
    // work (e.g. applying a provider) leaves it a disabled Send.
    connect(m_send, &QPushButton::clicked, this, [this]() {
        if (m_busy && m_streamReply)
            stopStream();
        else if (!m_busy)
            onSend();
    });
    connect(m_input, &QLineEdit::returnPressed, this, &AssistantPanel::onSend);
    connect(m_clear, &QPushButton::clicked, this, &AssistantPanel::onClearConversation);

    // Generic starters; a viewer can replace these with data-specific ones.
    setSuggestions({ u"Summarise what's loaded"_s,
                     u"What are the key statistics?"_s,
                     u"What can you help me with?"_s });
}

void AssistantPanel::setSuggestions(const QStringList &prompts)
{
    m_suggestionPrompts = prompts;
    rebuildSuggestions();
}

void AssistantPanel::rebuildSuggestions()
{
    if (!m_suggestionsLayout)
        return;
    // Clear existing chips.
    while (QLayoutItem *item = m_suggestionsLayout->takeAt(0)) {
        if (QWidget *w = item->widget())
            w->deleteLater();
        delete item;
    }
    auto *hint = new QLabel(u"Try asking:"_s, m_suggestions);
    hint->setStyleSheet(u"color: palette(mid); font-size: 11px;"_s);
    m_suggestionsLayout->addWidget(hint);
    for (const QString &p : m_suggestionPrompts) {
        auto *chip = new QPushButton(p, m_suggestions);
        chip->setCursor(Qt::PointingHandCursor);
        chip->setStyleSheet(
                u"QPushButton { text-align: left; padding: 5px 9px; border: 1px solid "
                u"palette(mid); border-radius: 10px; color: palette(text); "
                u"background: palette(base); }"
                u"QPushButton:hover { background: palette(alternate-base); }"_s);
        connect(chip, &QPushButton::clicked, this, [this, p]() {
            if (m_busy)
                return;
            m_input->setText(p);
            onSend();
        });
        m_suggestionsLayout->addWidget(chip);
    }
    // Only meaningful while the conversation is empty.
    m_suggestions->setVisible(m_entries.isEmpty() && !m_suggestionPrompts.isEmpty());
}

void AssistantPanel::setBackend(const QString &url, const QString &token)
{
    const bool endpointChanged = (url != m_url || token != m_token);
    m_url = url;
    m_token = token;
    if (endpointChanged) {
        m_providerAutoApplied = false;   // a new endpoint must get the saved provider
        // A new backend = a new session/datasets — the follow-up context no longer
        // applies. (Harmless on the first connect: nothing to clear.)
        if (!m_turns.isEmpty() || !m_entries.isEmpty()) {
            m_turns = QJsonArray{};
            m_entries.clear();
            if (m_transcript)
                renderConversation();
        }
    }
    // Re-apply whenever the user edits the copilot provider in the global Settings
    // dialog (it persists + emits updated()). A real member slot means
    // Qt::UniqueConnection actually de-dupes across repeated setBackend() calls.
    if (Settings *s = BackendClientFactory::instance()->settings())
        connect(s, &Settings::updated, this, &AssistantPanel::onSettingsUpdated,
                Qt::UniqueConnection);
    // Auto-apply the persisted provider so the user never re-enters it.
    this->applyProviderFromSettings();
}

void AssistantPanel::onSettingsUpdated()
{
    m_providerAutoApplied = false;   // force a re-apply on a settings change
    this->applyProviderFromSettings();
}

void AssistantPanel::applyProviderFromSettings()
{
    // Auto-apply only once per settings change, so a repeated or stale
    // connect-time POST can't overwrite what the user just chose. Editing the
    // provider in Settings clears the flag (see onSettingsUpdated).
    if (m_providerAutoApplied)
        return;
    Settings *s = BackendClientFactory::instance()->settings();
    if (!s || m_url.isEmpty())
        return;
    // Never overlap a query, a stream or another provider POST: they share
    // m_busy, the stream pointer and the reply buffer, and whichever finished
    // first would hand the panel back while the other was still running. The
    // apply is deferred instead of dropped — the user changed the provider in
    // Settings and expects it to take, not to be lost because a question
    // happened to be in flight. setBusy() re-runs this once the panel is free,
    // and it re-reads Settings then, so only the newest choice is applied.
    // m_streamReply as well as m_busy: the busy state is dropped on the SSE
    // "result" frame, while the reply itself stays open for a little longer.
    // Going by m_busy alone would start the POST inside that window, and the
    // reply's own finished handler would then clear busy under it.
    if (m_busy || m_streamReply) {
        m_providerApplyPending = true;
        return;
    }
    m_providerApplyPending = false;
    const int idx = s->getCopilotProvider();
    const QString key = s->getCopilotKey();
    const QString model = s->getCopilotModel();
    // Default "null" so a persisted None explicitly resets the backend provider.
    QString prov = u"null"_s, anthropicKey, openaiKey, baseUrl, baseUrlKey;
    if (idx == 0) {
        prov = u"anthropic"_s;
        anthropicKey = key;
    } else if (idx == 1) {
        prov = u"openai"_s;
        openaiKey = key;
    } else if (idx == 2) {
        prov = u"openai"_s;
        baseUrl = s->getCopilotUrl();
        baseUrlKey = key;
    }
    // The panel is busy until the backend has taken the provider: a question
    // asked in between would be answered by whatever provider the server still
    // had. A failure is reported and the flag is cleared, so the next connect or
    // Settings change tries again instead of silently keeping the old provider.
    m_providerAutoApplied = true;
    setBusy(true);
    m_status->setText(u"Applying provider…"_s);
    const QString url0 = m_url, token0 = m_token;
    auto *watcher = new QFutureWatcher<BackendAssistantConfigResult>(this);
    connect(watcher, &QFutureWatcher<BackendAssistantConfigResult>::finished, this,
            [this, watcher]() {
                const BackendAssistantConfigResult r = watcher->result();
                watcher->deleteLater();
                setBusy(false);
                if (!r.error.isEmpty()) {
                    m_providerAutoApplied = false;   // let the next attempt retry
                    appendError(r.error);
                    return;
                }
                m_status->setText(r.configured
                        ? u"Provider: %1 (ready)"_s.arg(r.provider)
                        : u"Provider: %1 (not configured)"_s.arg(r.provider));
            });
    watcher->setFuture(QtConcurrent::run(
            [url0, token0, prov, anthropicKey, openaiKey, baseUrl, baseUrlKey, model]() {
                BackendClient client(url0, token0);
                return client.setAssistantConfig(prov, anthropicKey, openaiKey, baseUrl,
                                                 baseUrlKey, model);
            }));
}

void AssistantPanel::setDatasetId(const QString &datasetId)
{
    if (datasetId == m_datasetId)
        return;
    // The conversation is ABOUT a specific dataset — when it changes (e.g. a
    // session-expiry reopen gives a new id), drop the follow-up context and
    // transcript so old Q&A can't be replayed against a different dataset.
    m_datasetId = datasetId;
    m_turns = QJsonArray{};
    m_entries.clear();
    renderConversation();
}

void AssistantPanel::maybeApplyPendingProvider()
{
    // A provider change that arrived mid-query runs once the panel is really
    // idle — no busy state AND no stream still open. Queued rather than called
    // directly, so it cannot re-enter its caller.
    if (!m_providerApplyPending || m_busy || m_streamReply)
        return;
    m_providerApplyPending = false;
    m_providerAutoApplied = false;
    QTimer::singleShot(0, this, &AssistantPanel::applyProviderFromSettings);
}

void AssistantPanel::setBusy(bool busy)
{
    if (m_busy == busy)
        return;
    m_busy = busy;
    if (!busy)
        maybeApplyPendingProvider();
    refreshSendButton();
    m_input->setEnabled(!busy);
    if (m_suggestions)
        m_suggestions->setEnabled(!busy);   // starter chips shouldn't look clickable mid-request
    m_status->setText(busy ? u"Thinking…"_s : QString());
    emit busyChanged(busy);
}

void AssistantPanel::appendError(const QString &text)
{
    Entry e;
    e.kind = Entry::Error;
    e.text = text;
    m_entries.push_back(e);
    renderConversation();
}

void AssistantPanel::onClearConversation()
{
    if (m_busy)
        return;
    m_entries.clear();
    m_turns = QJsonArray{};
    m_input->clear();
    renderConversation();
}

void AssistantPanel::renderConversation()
{
    const BubbleTheme t = bubbleTheme(this->palette());

    // A right- or left-aligned chat bubble, laid out with a 2-cell outer table so
    // alignment renders reliably in QTextDocument (no flexbox / border-radius).
    auto bubble = [](bool rightAlign, const QString &bg, const QString &inner) {
        const QString gap = u"<td width='16%'></td>"_s;
        const QString cell =
                u"<td width='84%'><table width='100%' cellpadding='8' cellspacing='0' "
                u"bgcolor='%1'><tr><td>%2</td></tr></table></td>"_s.arg(bg, inner);
        return u"<table width='100%' cellspacing='0' cellpadding='0' "
               u"style='margin:5px 0;'><tr>%1</tr></table>"_s
                .arg(rightAlign ? (gap + cell) : (cell + gap));
    };

    QString html = u"<div style='font-family:sans-serif;'>"_s;
    for (int i = 0; i < m_entries.size(); ++i) {
        const Entry &e = m_entries.at(i);
        if (e.kind == Entry::User) {
            const QString inner =
                    u"<span style='color:%1;font-size:small;'>You</span><br>"
                    u"<span style='color:%2;'>%3</span>"_s
                            .arg(t.meta, t.userFg, esc(e.text).replace(u"\n"_s, u"<br>"_s));
            html += bubble(/*rightAlign=*/true, t.userBg, inner);
        } else if (e.kind == Entry::Assistant) {
            QString inner = u"<span style='color:%1;font-size:small;'>Copilot"_s.arg(t.meta);
            if (!e.provider.isEmpty())
                inner += u" · %1"_s.arg(esc(e.provider));
            inner += u"</span><br><span style='color:%1;'>%2</span>"_s
                             .arg(t.botFg, esc(e.text).replace(u"\n"_s, u"<br>"_s));
            if (!e.configured) {
                inner += u"<br><span style='color:%1;font-size:small;'><i>No LLM provider is "
                         u"configured on the server - the analysis tools still work; choose one "
                         u"in Settings ▸ Scientific Copilot to enable "
                         u"natural-language answers.</i></span>"_s.arg(t.meta);
            }
            if (!e.provenance.isEmpty()) {
                inner += u"<div style='margin-top:6px;border-top:1px solid %1;padding-top:4px;'>"
                         u"<span style='color:%2;font-size:small;'>%3 · grounded in tool call(s):"
                         u"</span><div style='color:%2;font-size:small;'>%4</div></div>"_s
                                 .arg(t.rule, t.meta,
                                      e.grounded ? u"✓ provenance"_s : u"provenance"_s,
                                      e.provenance);
            }
            if (!e.repro.isEmpty()) {
                inner += u"<div style='margin-top:4px;'><span style='color:%1;font-size:small;'>"
                         u"Reproducibility</span><pre style='color:%1;font-size:small;margin:2px 0;'>"
                         u"%2</pre></div>"_s.arg(t.meta, esc(e.repro));
            }
            // Copy affordances (handled via anchorClicked, like the Undo link).
            inner += u"<div style='margin-top:6px;'>"
                     u"<a href='copilot:copy:%1' style='color:%2;font-size:small;'>⧉ Copy</a>"_s
                             .arg(QString::number(i), t.meta);
            if (!e.repro.isEmpty())
                inner += u"&nbsp;&nbsp;<a href='copilot:copyrepro:%1' "
                         u"style='color:%2;font-size:small;'>⧉ Copy script</a>"_s
                                 .arg(QString::number(i), t.meta);
            inner += u"</div>"_s;
            html += bubble(/*rightAlign=*/false, t.botBg, inner);
        } else if (e.kind == Entry::ActionLog) {
            html += u"<table width='100%' cellspacing='0' cellpadding='6' style='margin:2px 0;'>"
                    u"<tr><td><span style='color:%1;font-size:small;'>▸ Applied to the view "
                    u"[<a href='copilot:undo' style='color:%1;'>↩ Undo latest change</a>]</span>"
                    u"<span style='color:%1;font-size:small;'>%2</span></td></tr></table>"_s
                            .arg(t.meta, e.actionLog);
        } else {   // Error
            html += bubble(/*rightAlign=*/false, t.errBg,
                           u"<span style='color:%1;'><b>Error:</b> %2</span>"_s
                                   .arg(t.errFg, esc(e.text).replace(u"\n"_s, u"<br>"_s)));
        }
    }
    html += u"</div>"_s;

    m_transcript->setHtml(html);
    // Keep the newest message in view.
    m_transcript->verticalScrollBar()->setValue(m_transcript->verticalScrollBar()->maximum());
    // Starter chips are a discoverability aid — only show them on an empty chat.
    if (m_suggestions)
        m_suggestions->setVisible(m_entries.isEmpty() && !m_suggestionPrompts.isEmpty());
}

void AssistantPanel::onSend()
{
    if (m_busy)
        return;
    const QString question = m_input->text().trimmed();
    if (question.isEmpty())
        return;
    if (m_url.isEmpty()) {
        appendError(u"No backend connection."_s);
        return;
    }
    { Entry e; e.kind = Entry::User; e.text = question; m_entries.push_back(e); }
    renderConversation();
    m_input->clear();
    setBusy(true);
    startStream(question);
}

// Close a stream that is still open, so nothing of it reaches the next one.
//
// The busy state is dropped on the SSE "result" frame, while the reply stays
// open until the server closes it, so Send is reachable with the previous
// stream still live. Left alone, its late frames would be appended to the new
// answer, and its finished handler would clear the new stream's busy state.
void AssistantPanel::closeLiveStream()
{
    if (!m_streamReply)
        return;
    QNetworkReply *stale = m_streamReply;
    m_streamReply = nullptr;
    stale->disconnect(this);     // nothing of it touches the new conversation
    stale->abort();
    stale->deleteLater();
}

void AssistantPanel::startStream(const QString &question)
{
    if (!m_net)
        m_net = new QNetworkAccessManager(this);
    // Before the resets below, not after: on the session-minting path the send
    // is deferred to a worker, and a surviving reply would otherwise spend that
    // time writing into the buffer and flags this just cleared.
    closeLiveStream();
    // Capture the scope so a reply that outlives a dataset/backend swap is dropped.
    m_scopeDatasetId = m_datasetId;
    m_scopeUrl = m_url;
    m_scopeToken = m_token;
    m_streamBuf.clear();
    m_streamGotResult = false;
    m_userStopped = false;

    // A strict backend refuses a client-invented session id, so the stream must
    // not go out before this backend has minted one. The mint is a network call:
    // run it on a worker thread and send afterwards, rather than freezing the UI.
    if (!BackendClient::sessionResolvedForUrl(m_url, m_token)) {
        const QString url = m_url, token = m_token;
        using Mint = BackendClient::SessionMint;
        struct MintOutcome
        {
            Mint mint{ Mint::Deferred };
            QString token;      ///< identity that actually authenticated
        };
        auto *watcher = new QFutureWatcher<MintOutcome>(this);
        connect(watcher, &QFutureWatcher<MintOutcome>::finished, this,
                [this, watcher, question, url, token]() {
                    const MintOutcome res = watcher->result();
                    const Mint outcome = res.mint;
                    watcher->deleteLater();
                    // Drop it if the conversation scope changed while we waited.
                    if (m_url != url || m_token != token) {
                        setBusy(false);
                        return;
                    }
                    // The mint may have fallen back to a refreshed token; adopt it
                    // so our hand-built request sends a matching auth + session.
                    if (!res.token.isEmpty() && res.token != m_token) {
                        m_token = res.token;
                        m_scopeToken = res.token;
                    }
                    if (outcome == Mint::Deferred) {
                        // We could not obtain a session; a strict backend would
                        // reject the stream, so say so instead of failing oddly.
                        setBusy(false);
                        appendError(u"Could not establish a session with the backend - "
                                    u"check the connection and try again."_s);
                        return;
                    }
                    sendStreamRequest(question);
                });
        watcher->setFuture(QtConcurrent::run([url, token]() {
            MintOutcome out;
            QString effective;
            out.mint = BackendClient::ensureSessionFor(url, token, &effective);
            out.token = effective;
            return out;
        }));
        return;
    }
    sendStreamRequest(question);
}

void AssistantPanel::sendStreamRequest(const QString &question)
{
    QJsonObject body{ { u"question"_s, question } };
    if (!m_datasetId.isEmpty())
        body.insert(u"dataset_id"_s, m_datasetId);
    if (!m_turns.isEmpty())
        body.insert(u"history"_s, m_turns);   // prior turns → follow-up context

    QNetworkRequest req{ QUrl(m_url + u"/v1/assistant/stream"_s) };
    // Scope the stream to this desktop's private session so it resolves the same
    // datasets the viewers opened. The stamper also normalises the token through
    // any rotation the mint recorded — on the fast path (session already
    // resolved) we never ran ensureSessionFor(), so m_token may be the old one
    // and the pair on the wire would not match.
    BackendClient::applySessionHeaders(req, m_url, m_token);
    req.setHeader(QNetworkRequest::ContentTypeHeader, u"application/json"_s);

    closeLiveStream();   // normally a no-op: startStream() already did it
    QNetworkReply *reply = m_net->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    m_streamReply = reply;
    refreshSendButton();   // now that a stream is live, the button offers Stop
    connect(reply, &QNetworkReply::readyRead, this, [this, reply, question]() {
        if (reply != m_streamReply || m_userStopped)
            return;   // ignore data once the user has cancelled this stream
        m_streamBuf += reply->readAll();
        handleStreamBuffer(question);
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, question]() {
        // A stale reply (a newer stream already started) must not touch current
        // UI state — just clean it up.
        if (reply != m_streamReply) {
            reply->deleteLater();
            return;
        }
        const bool ok = (reply->error() == QNetworkReply::NoError);
        const QString netErr = reply->errorString();
        // Drain any final bytes (the last SSE frame, incl. the result, can land
        // with 'finished' without a separate readyRead) — but NOT after a Stop,
        // which must not apply a late result/actions.
        if (!m_userStopped)
            m_streamBuf += reply->readAll();
        m_streamReply = nullptr;
        reply->deleteLater();
        setBusy(false);
        // setBusy() may have had nothing to clear (the "result" frame already
        // did), so the pending provider change is flushed from here too.
        maybeApplyPendingProvider();
        // A user-initiated Stop: drop any buffered bytes, no error banner.
        if (m_userStopped) {
            m_streamBuf.clear();
            m_status->setText(u"Stopped."_s);
            return;
        }
        handleStreamBuffer(question);
        // Surface a network-level failure only if no result/error was delivered
        // and the scope is still current.
        if (m_streamGotResult)
            return;
        if (m_scopeDatasetId != m_datasetId || m_scopeUrl != m_url || m_scopeToken != m_token)
            return;
        appendError(ok ? u"The copilot stream ended without an answer."_s : netErr);
    });
}

void AssistantPanel::handleStreamBuffer(const QString &question)
{
    // Parse each complete "event: X\ndata: Y\n\n" SSE block.
    int idx;
    while ((idx = m_streamBuf.indexOf("\n\n")) >= 0) {
        const QByteArray block = m_streamBuf.left(idx);
        m_streamBuf.remove(0, idx + 2);
        QByteArray event, data;
        for (const QByteArray &line : block.split('\n')) {
            if (line.startsWith("event:"))
                event = line.mid(6).trimmed();
            else if (line.startsWith("data:"))
                data = line.mid(5).trimmed();
        }
        const QJsonObject obj = QJsonDocument::fromJson(data).object();
        if (event == "tool") {
            const QString tool = obj.value(u"tool"_s).toString();
            m_status->setText(tool.isEmpty() ? u"Thinking…"_s : u"Running %1…"_s.arg(tool));
        } else if (event == "thinking") {
            m_status->setText(u"Thinking…"_s);
        } else if (event == "result") {
            m_streamGotResult = true;
            setBusy(false);
            // Discard a result whose scope changed while it streamed.
            if (m_scopeDatasetId != m_datasetId || m_scopeUrl != m_url || m_scopeToken != m_token)
                return;
            applyResult(BackendClient::parseAssistantResult(obj), question);
        } else if (event == "error") {
            m_streamGotResult = true;
            setBusy(false);
            if (m_scopeDatasetId == m_datasetId && m_scopeUrl == m_url && m_scopeToken == m_token) {
                const QString err = obj.value(u"error"_s).toString();
                appendError(err.isEmpty() ? u"The copilot stream reported an error."_s : err);
            }
        }
    }
}

void AssistantPanel::applyResult(const BackendAssistantResult &r, const QString &question)
{
    if (!r.error.isEmpty()) {
        appendError(r.error);
        return;
    }
    const BubbleTheme t = bubbleTheme(this->palette());
    QString prov;
    for (const BackendAssistantToolCall &c : r.toolCalls) {
        prov += u"<div><code>%1(%2)</code>%3</div>"_s.arg(
                esc(c.name), esc(c.argumentsJson),
                c.error.isEmpty()
                        ? u" → <span style='color:%1'>%2</span>"_s.arg(t.meta, esc(c.resultJson))
                        : u" → <span style='color:%1'>%2</span>"_s.arg(t.errFg, esc(c.error)));
    }
    Entry a;
    a.kind = Entry::Assistant;
    a.text = r.answer;
    a.provider = r.provider;
    a.configured = r.configured;
    a.grounded = r.toolCallsRecorded;
    a.repro = r.reproducibilityScript;
    a.provenance = prov;
    m_entries.push_back(a);
    if (!r.actions.isEmpty()) {
        QString log;
        for (const QJsonObject &act : r.actions) {
            QString desc = act.value(u"type"_s).toString();
            for (auto it = act.constBegin(); it != act.constEnd(); ++it) {
                if (it.key() == u"type"_s)
                    continue;
                // Skip bulky payloads (e.g. a pinned spectrum's samples).
                if (it.value().isArray() || it.value().isObject())
                    continue;
                desc += u" %1=%2"_s.arg(it.key(), it.value().toVariant().toString());
            }
            log += u" · %1"_s.arg(esc(desc));
        }
        Entry le; le.kind = Entry::ActionLog; le.actionLog = log;
        m_entries.push_back(le);
        // Route each directive to its target view (the active one by default) —
        // the copilot can act across several open views at once (Phase 2).
        CopilotActionRouter::instance()->dispatch(r.actions, m_datasetId);
    }
    // Record this exchange as follow-up context; clamp each turn to the server's
    // per-turn cap (4000) and cap the retained turn count so requests stay bounded.
    m_turns.push_back(QJsonObject{ { u"role"_s, u"user"_s }, { u"text"_s, question.left(4000) } });
    m_turns.push_back(QJsonObject{ { u"role"_s, u"assistant"_s },
                                   { u"text"_s, r.answer.left(4000) } });
    while (m_turns.size() > 20)
        m_turns.removeFirst();
    renderConversation();
}

void AssistantPanel::refreshSendButton()
{
    const bool streaming = m_busy && m_streamReply != nullptr;
    m_send->setText(streaming ? u"Stop"_s : u"Send"_s);
    // Enabled to Stop while streaming, or to Send while idle; disabled for other
    // busy work (e.g. applying a provider), where there is nothing to cancel.
    m_send->setEnabled(streaming || !m_busy);
}

void AssistantPanel::stopStream()
{
    if (m_userStopped || !m_streamReply)
        return;   // nothing streaming to cancel (or already cancelling)
    m_userStopped = true;
    m_status->setText(u"Stopping…"_s);
    m_streamReply->abort();   // → finished fires; the "Stopped." note is shown there
}
