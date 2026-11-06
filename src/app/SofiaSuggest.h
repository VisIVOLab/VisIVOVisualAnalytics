#ifndef SOFIASUGGEST_H
#define SOFIASUGGEST_H

// SofiaSuggest — the client half of the SoFiA-2 parameter suggestion (issue
// #64): the answer of POST /v1/sofia2/suggest/{dataset_id} read into rows,
// the editable parameter set the dialog shows (the user's edits survive a new
// suggestion), what a run sends to POST /v1/tasks/sourcefind and what the
// source-finding product records. QtCore only, so it is unit-tested without a
// widget and its request names are checked against the service's models.

#include "ProductRegistry.h"

#include <QJsonObject>
#include <QPair>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QVector>

namespace visivo {

/// A deduction of the suggester the user may have to confirm: the data type
/// (emission / absorption / continuum) or the line-width ladder (hi /
/// galactic). Ask when `ambiguous`, pre-selecting `value` among `options`.
struct SofiaChoice
{
    bool present{ false };    ///< false when the answer carried none
    QString value;
    bool deduced{ false };
    bool ambiguous{ false };
    QString reason;
    QString confidence;       ///< high | medium | low | user; empty for the line
    QStringList options;
};

/// One suggested parameter, as the service sends it.
struct SofiaSuggestedParam
{
    QString key;              ///< SoFiA-2 name, e.g. "scfind.kernelsXY"
    QString value;            ///< as written in the .par, e.g. "0, 5, 10"
    QString reason;
    QString sofiaDefault;     ///< SoFiA-2 2.7's built-in value
    bool hasDefault{ false }; ///< false when SoFiA has none (input.region)
    bool changed{ false };    ///< the value differs from that default
};

struct SofiaSuggestion
{
    bool valid{ false };
    QString error;
    QString sofiaVersion;
    SofiaChoice dataType;
    SofiaChoice line;
    QVector<SofiaSuggestedParam> parameters;   ///< in display order
    /// SoFiA-2 2.7's default of every key a run accepts ("" when it has
    /// none); empty from a backend that does not send it.
    QMap<QString, QString> sofiaDefaults;
    QVariantMap measurements;
    QVector<QPair<QString, QString>> warnings; ///< code, message
};

/// What the dialog asks the suggester: the user's answers to its two
/// questions (empty = let it deduce).
struct SofiaAsk
{
    QString dataType;
    QString line;
};

/// The ask on its way and the one the table was suggested for. A request
/// that brings no suggestion leaves the table as it was, so the ask (and the
/// combos showing it) must go back to the one that table answers, or Run
/// would run old parameters under a choice they were not made for.
struct SofiaAskState
{
    SofiaAsk pending;
    SofiaAsk accepted;
    bool hasAccepted{ false };

    void succeeded()
    {
        accepted = pending;
        hasAccepted = true;
    }
    void failed()
    {
        if (hasAccepted)
            pending = accepted;
    }
};

/// The suggest route's JSON answer.
SofiaSuggestion parseSofiaSuggestion(const QJsonObject &body);

/// The body of a suggest request. An empty @p dataType or @p line is sent as
/// null (the service deduces it); a value is the user's choice.
QJsonObject sofiaSuggestRequest(const QString &dataType, const QString &line);

/// What a refused request said, for the user: FastAPI's `detail` when it is
/// text, the messages of a validation error list, else @p raw as it came.
QString backendErrorDetail(const QString &raw);

/// What to tell the user about a failed request: for an HTTP refusal
/// (@p status >= 400) the reason in the response @p body, which is where
/// BackendClient::performPost leaves it (its error string is Qt's "server
/// replied: Unprocessable Entity"); otherwise, or with an empty body,
/// @p transportError.
QString refusalText(int status, const QByteArray &body, const QString &transportError);

/// The measurements in one line, for the dialog: σ, beam, channel width,
/// channel correlation, resolution elements, shape. Missing ones are left out.
QString sofiaMeasurementLine(const QVariantMap &m);

/// SoFiA's own counts after a run (the task result's `summary`).
struct SofiaRunSummary
{
    bool present{ false };
    int nPositive{ -1 };      ///< -1: reliability did not run
    int nNegative{ -1 };
    bool hasRho{ false };
    double rho{ 0.0 };
    int nAfterFilter{ -1 };
    QStringList warnings;
    QStringList errors;       ///< SoFiA's ERROR: messages (exit 8: no sources left)
    QString note;
};

/// What the task result says about how SoFiA-2 ended: 0, or 8 for "no
/// sources left after filtering" (a completed, empty result); -1 when the
/// result carries no exit code (an older backend).
int sofiaExitCode(const QJsonObject &result);

/// The dialog's first line for a completed run: the count, or for an empty
/// result that nothing was left after filtering.
QString sofiaRunHeadline(int nSources, int exitCode);

/// The dialog's text for a run that failed. @p refusedStatus is the HTTP
/// status of a refused task creation (a parameter the backend refused: 422),
/// which SoFiA-2 never started; 0 once a task exists. A task's own failure
/// is shown as the backend words it ("SoFiA-2 failed (exit 7): …"); anything
/// else that ended a task (a lost task, a backend error) is not said to be
/// either.
QString sofiaFailureText(const QString &error, int refusedStatus);

SofiaRunSummary parseSofiaRunSummary(const QJsonObject &summary);

/// The summary in a few sentences, for the dialog and the product's record.
QString sofiaRunSummaryText(const SofiaRunSummary &s);

/// The parameters of the dialog: the suggestion, the user's edits on top of
/// it, and the proposals that need a yes before they are sent.
class SofiaParamSet
{
public:
    struct Row
    {
        QString key;
        QString value;          ///< what runs
        QString suggested;      ///< empty when the latest suggestion has no such key
        QString reason;
        QString sofiaDefault;
        bool hasDefault{ false };
        /// false for a key the user added that the backend gave no default
        /// for: the default is not known, rather than "none".
        bool defaultKnown{ true };
        /// The suggestion differs from SoFiA's default (the service's own
        /// comparison: "4.0" and "4" are the same number to it).
        bool suggestedChanged{ false };
        bool edited{ false };   ///< the user set a value other than the suggestion
        bool needsConfirmation{ false };
        bool confirmed{ false };
    };

    /// A new suggestion: values the user edited stay as they are (the user's
    /// choice wins), everything else takes the new value. A key the user
    /// edited stays even when the new suggestion drops it; any other key the
    /// suggestion no longer has goes. A proposal keeps its yes only while its
    /// value is unchanged.
    void applySuggestion(const QVector<SofiaSuggestedParam> &params);

    /// The user typed @p value for @p key. Typing the suggested value back
    /// clears the edit. Typing into a proposal confirms it: the value is the
    /// user's. False for an unknown key.
    bool setValue(const QString &key, const QString &value);

    /// Yes or no to a proposal (input.region).
    bool setConfirmed(const QString &key, bool confirmed);

    /// Every edit back to the suggestion; keys only the user kept go.
    void resetToSuggested();

    /// The keys a run accepts, with SoFiA's default of each (the suggestion's
    /// `sofia_defaults`). Empty: not known, every `section.key` is taken and
    /// the backend checks it at the run.
    void setSofiaDefaults(const QMap<QString, QString> &defaults) { m_defaults = defaults; }
    /// @p key is one a run accepts, as far as the client knows.
    bool acceptsKey(const QString &key) const;

    /// A parameter the suggestion does not set (e.g. dilation.*), added by
    /// the user: kept like any edit, sent while it has a value. False when
    /// the key is empty, not `section.key`, not one a run accepts
    /// (acceptsKey), or already a row (edit it there).
    bool addUserParam(const QString &key, const QString &value);

    /// What the dialog's Status column says about a row: a proposal waiting
    /// for a yes, a blank (SoFiA's default runs), the user's value, or how the
    /// suggestion compares with SoFiA's default.
    static QString rowStatus(const Row &r);
    /// The row's value differs from SoFiA's default (shown emphasised).
    static bool differsFromDefault(const Row &r);

    const QVector<Row> &rows() const { return m_rows; }
    int indexOf(const QString &key) const;
    bool anyEdited() const;
    /// A proposal waiting for the user's yes.
    bool hasUnconfirmed() const;

    /// What POST /v1/tasks/sourcefind is sent as `params`: every row with a
    /// value, except proposals not confirmed. Values are the .par text.
    QJsonObject runParams() const;

    /// The Provenance lines of a run: key → "value (suggested)", "value
    /// (edited; suggested x)", "value (set by you)", "value (proposal declined,
    /// not sent)".
    QVariantMap provenance() const;

private:
    QVector<Row> m_rows;
    QMap<QString, QString> m_defaults;
};

/// A SoFiA-2 label mask as the backend writes it (`sofia_out_mask.fits`, the
/// name both finder routes use): integer labels 1..N on zeros, which a
/// Composite rendering with a noise-based opacity ramp leaves almost
/// invisible, so the cube viewer opens it in MIP.
bool isSofiaMaskPath(const QString &path);

/// The catalogue's columns in display order: `id` and `name` first (what a
/// row is), then the rest alphabetically.
QStringList sofiaCatalogueColumns(const QStringList &keys);

/// The Session Data label of a run, distinct for two runs of the same
/// parameters: count, how many values the user edited, finish time
/// ("SoFiA-2 · 12 sources · 14:32:05").
QString sofiaRunLabel(int nSources, int edited, const QString &finishedAt);

/// SoFiA-2 needs the user's yes before this key is sent: a sub-cube proposed
/// because the cube does not fit the node's memory.
bool sofiaNeedsConfirmation(const QString &key);

/// The replay of a source-finding product: the task route with the request
/// the client sent, under the API's field names.
ReplayStep sofiaSourceFindStep(const QString &datasetId, const QJsonObject &params);

} // namespace visivo

#endif // SOFIASUGGEST_H
