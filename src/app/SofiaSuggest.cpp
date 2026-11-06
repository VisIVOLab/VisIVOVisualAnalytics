#include "SofiaSuggest.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QRegularExpression>

#include <algorithm>
#include <cmath>

using namespace Qt::StringLiterals;

namespace visivo {

namespace {

SofiaChoice parseChoice(const QJsonValue &v)
{
    SofiaChoice c;
    if (!v.isObject())
        return c;
    const QJsonObject o = v.toObject();
    c.present = true;
    c.value = o.value(u"value"_s).toString();
    c.deduced = o.value(u"deduced"_s).toBool();
    c.ambiguous = o.value(u"ambiguous"_s).toBool();
    c.reason = o.value(u"reason"_s).toString();
    c.confidence = o.value(u"confidence"_s).toString();
    for (const QJsonValue &opt : o.value(u"options"_s).toArray())
        c.options << opt.toString();
    // The pre-selection must be one of the choices offered.
    if (!c.value.isEmpty() && !c.options.contains(c.value))
        c.options.prepend(c.value);
    return c;
}

/// A measurement that is a finite number; false for null or absent.
bool number(const QVariantMap &m, const QString &key, double *out)
{
    const QVariant v = m.value(key);
    if (!v.isValid() || v.isNull())
        return false;
    bool ok = false;
    const double d = v.toDouble(&ok);
    if (!ok || !std::isfinite(d))
        return false;
    *out = d;
    return true;
}

/// 1262000 → "1.26e6": the exponent without the sign and padding of 'g'.
QString compact(double v)
{
    QString s = QString::number(v, 'g', 3);
    s.replace(u"e+0"_s, u"e"_s).replace(u"e+"_s, u"e"_s).replace(u"e-0"_s, u"e-"_s);
    return s;
}

} // namespace

SofiaSuggestion parseSofiaSuggestion(const QJsonObject &body)
{
    SofiaSuggestion s;
    s.valid = body.value(u"valid"_s).toBool(false);
    s.error = body.value(u"error"_s).toString();
    s.sofiaVersion = body.value(u"sofia_version"_s).toString();
    s.dataType = parseChoice(body.value(u"data_type"_s));
    s.line = parseChoice(body.value(u"line"_s));
    for (const QJsonValue &v : body.value(u"parameters"_s).toArray()) {
        const QJsonObject o = v.toObject();
        SofiaSuggestedParam p;
        p.key = o.value(u"key"_s).toString();
        if (p.key.isEmpty())
            continue;
        p.value = o.value(u"value"_s).toString();
        p.reason = o.value(u"reason"_s).toString();
        const QJsonValue def = o.value(u"sofia_default"_s);
        p.hasDefault = def.isString();
        p.sofiaDefault = def.toString();
        p.changed = o.value(u"changed"_s).toBool(false);
        s.parameters.append(p);
    }
    const QJsonObject defaults = body.value(u"sofia_defaults"_s).toObject();
    for (auto it = defaults.begin(); it != defaults.end(); ++it)
        s.sofiaDefaults.insert(it.key(), it.value().toString());
    s.measurements = body.value(u"measurements"_s).toObject().toVariantMap();
    for (const QJsonValue &v : body.value(u"warnings"_s).toArray()) {
        const QJsonObject o = v.toObject();
        const QString message = o.value(u"message"_s).toString();
        if (!message.isEmpty())
            s.warnings.append({ o.value(u"code"_s).toString(), message });
    }
    if (s.valid && s.parameters.isEmpty()) {
        s.valid = false;
        if (s.error.isEmpty())
            s.error = u"The backend suggested no parameters."_s;
    }
    return s;
}

QJsonObject sofiaSuggestRequest(const QString &dataType, const QString &line)
{
    return QJsonObject{
        { u"data_type"_s, dataType.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(dataType) },
        { u"line"_s, line.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(line) },
    };
}

QString backendErrorDetail(const QString &raw)
{
    const QJsonDocument doc = QJsonDocument::fromJson(raw.toUtf8());
    if (!doc.isObject())
        return raw.trimmed();
    const QJsonValue detail = doc.object().value(u"detail"_s);
    if (detail.isString())
        return detail.toString();
    if (detail.isArray()) {
        // A request the model refused: one message per field, with where.
        QStringList messages;
        for (const QJsonValue &item : detail.toArray()) {
            const QJsonObject o = item.toObject();
            QStringList where;
            for (const QJsonValue &part : o.value(u"loc"_s).toArray())
                if (part.isString() && part.toString() != u"body"_s)
                    where << part.toString();
            const QString msg = o.value(u"msg"_s).toString();
            if (!msg.isEmpty())
                messages << (where.isEmpty() ? msg : where.join(u'.') + u": "_s + msg);
        }
        if (!messages.isEmpty())
            return messages.join(u"; "_s);
    }
    const QString error = doc.object().value(u"error"_s).toString();
    return error.isEmpty() ? raw.trimmed() : error;
}

QString refusalText(int status, const QByteArray &body, const QString &transportError)
{
    if (status < 400 || body.trimmed().isEmpty())
        return transportError;
    const QString detail = backendErrorDetail(QString::fromUtf8(body));
    return detail.isEmpty() ? transportError : detail;
}

QString sofiaMeasurementLine(const QVariantMap &m)
{
    QStringList parts;
    double v = 0.0;
    if (number(m, u"sigma"_s, &v)) {
        const QString unit = m.value(u"bunit"_s).toString();
        const bool positiveSide = m.value(u"noise_side"_s).toString() == u"positive"_s;
        parts << u"σ %1%2%3"_s.arg(compact(v), unit.isEmpty() ? QString() : u" "_s + unit,
                                   positiveSide ? u" (positive side)"_s : QString());
    }
    if (number(m, u"b_px"_s, &v))
        parts << u"beam %1 px"_s.arg(QString::number(v, 'f', 1));
    else
        parts << u"no beam in the header"_s;
    if (number(m, u"channel_kms"_s, &v))
        parts << u"Δv %1 km/s"_s.arg(compact(v));
    else
        parts << u"channel width unknown"_s;
    if (number(m, u"rho"_s, &v))
        parts << u"ρ %1"_s.arg(QString::number(v, 'f', 2));
    if (number(m, u"n_res"_s, &v))
        parts << u"N_res %1"_s.arg(compact(v));
    double rc = 0.0, rs = 0.0;
    const bool hasRc = number(m, u"r_chan"_s, &rc);
    const bool hasRs = number(m, u"r_sky"_s, &rs);
    if (hasRc || hasRs) {
        QStringList r;
        if (hasRc)
            r << u"×%1 over the band"_s.arg(QString::number(rc, 'f', 2));
        if (hasRs)
            r << u"×%1 across the field"_s.arg(QString::number(rs, 'f', 2));
        parts << u"noise varies "_s + r.join(u", "_s);
    }
    if (number(m, u"f_nan"_s, &v) && v > 0.0)
        parts << u"%1 % blank"_s.arg(QString::number(100.0 * v, 'g', 2));
    const QVariantList shape = m.value(u"shape"_s).toList();
    if (shape.size() == 3)
        parts << u"%1×%2×%3 (z×y×x)"_s.arg(shape[0].toLongLong()).arg(shape[1].toLongLong()).arg(
                shape[2].toLongLong());
    return parts.join(u" · "_s);
}

SofiaRunSummary parseSofiaRunSummary(const QJsonObject &summary)
{
    SofiaRunSummary s;
    if (summary.isEmpty())
        return s;
    s.present = true;
    const auto count = [&summary](const QString &key) {
        const QJsonValue v = summary.value(key);
        return v.isDouble() ? v.toInt(-1) : -1;
    };
    s.nPositive = count(u"n_positive"_s);
    s.nNegative = count(u"n_negative"_s);
    s.nAfterFilter = count(u"n_after_filter"_s);
    const QJsonValue rho = summary.value(u"rho"_s);
    s.hasRho = rho.isDouble();
    s.rho = rho.toDouble();
    for (const QJsonValue &w : summary.value(u"warnings"_s).toArray())
        if (!w.toString().isEmpty())
            s.warnings << w.toString();
    for (const QJsonValue &e : summary.value(u"errors"_s).toArray())
        if (!e.toString().isEmpty())
            s.errors << e.toString();
    s.note = summary.value(u"note"_s).toString();
    return s;
}

int sofiaExitCode(const QJsonObject &result)
{
    const QJsonValue v = result.value(u"exit_code"_s);
    return v.isDouble() ? v.toInt(-1) : -1;
}

QString sofiaRunHeadline(int nSources, int exitCode)
{
    // Exit 8 is SoFiA's "No sources left after filtering": a result, not a
    // failure (the inversion test of a setting expects exactly this).
    if (nSources == 0 && exitCode == 8)
        return u"No sources left after filtering."_s;
    return nSources == 1 ? u"1 source found."_s : u"%1 sources found."_s.arg(nSources);
}

QString sofiaRunSummaryText(const SofiaRunSummary &s)
{
    if (!s.present)
        return {};
    QStringList parts;
    if (!s.note.isEmpty())
        parts << s.note;
    else if (s.nNegative >= 0)
        parts << u"Reliability ran on %1 negative and %2 positive detections."_s.arg(s.nNegative).arg(
                s.nPositive);
    if (s.nAfterFilter >= 0)
        parts << u"%1 source(s) left after reliability and filtering."_s.arg(s.nAfterFilter);
    if (s.hasRho)
        parts << u"SoFiA measured an adjacent-channel noise correlation of %1."_s.arg(
                QString::number(s.rho, 'f', 3));
    // An error the note already says ("No sources left after filtering.")
    // is not repeated.
    QStringList errors;
    for (const QString &e : s.errors)
        if (s.note.isEmpty() || !e.contains(s.note.chopped(s.note.endsWith(u'.') ? 1 : 0)))
            errors << e;
    // SoFiA's messages are first sentences, some without their full stop:
    // each is closed so two do not run together.
    auto sentences = [](const QStringList &messages) {
        QStringList closed;
        for (const QString &m : messages)
            closed << (m.endsWith(u'.') || m.endsWith(u'!') || m.endsWith(u'?') ? m : m + u'.');
        return closed.join(u" "_s);
    };
    if (!errors.isEmpty())
        parts << u"SoFiA reported: %1"_s.arg(sentences(errors));
    if (!s.warnings.isEmpty())
        parts << u"SoFiA warned: %1"_s.arg(sentences(s.warnings));
    return parts.join(u" "_s);
}

QString sofiaFailureText(const QString &error, int refusedStatus)
{
    if (refusedStatus > 0)
        return u"SoFiA-2 did not run.\n%1"_s.arg(error);
    // The backend words a failure of SoFiA itself "SoFiA-2 failed (exit N)"
    // or "SoFiA-2 timed out", which say it all.
    if (error.startsWith(u"SoFiA-2 "_s))
        return error;
    return u"The run did not finish.\n%1"_s.arg(error);
}

bool isSofiaMaskPath(const QString &path)
{
    // The last path component, whichever separator the backend's OS uses.
    const qsizetype cut = std::max(path.lastIndexOf(u'/'), path.lastIndexOf(u'\\'));
    return path.mid(cut + 1).compare(u"sofia_out_mask.fits"_s, Qt::CaseInsensitive) == 0;
}

QStringList sofiaCatalogueColumns(const QStringList &keys)
{
    QStringList rest;
    for (const QString &k : keys)
        if (!rest.contains(k))
            rest << k;
    rest.sort();
    QStringList out;
    for (const QString &first : { u"id"_s, u"name"_s }) {
        if (rest.removeOne(first))
            out << first;
    }
    return out + rest;
}

QString sofiaRunLabel(int nSources, int edited, const QString &finishedAt)
{
    QStringList parts{ u"SoFiA-2"_s, nSources == 1 ? u"1 source"_s : u"%1 sources"_s.arg(nSources) };
    if (edited > 0)
        parts << u"%1 edited"_s.arg(edited);
    if (!finishedAt.isEmpty())
        parts << finishedAt;
    return parts.join(u" · "_s);
}

bool sofiaNeedsConfirmation(const QString &key)
{
    return key == u"input.region"_s;
}

void SofiaParamSet::applySuggestion(const QVector<SofiaSuggestedParam> &params)
{
    QVector<Row> next;
    next.reserve(params.size());
    for (const SofiaSuggestedParam &p : params) {
        Row r;
        r.key = p.key;
        r.suggested = p.value;
        r.value = p.value;
        r.reason = p.reason;
        r.sofiaDefault = p.sofiaDefault;
        r.hasDefault = p.hasDefault;
        r.suggestedChanged = p.changed;
        r.needsConfirmation = sofiaNeedsConfirmation(p.key);
        const int old = indexOf(p.key);
        if (old >= 0) {
            const Row &prev = m_rows[old];
            if (prev.edited && prev.value != p.value) {
                r.value = prev.value;
                r.edited = true;
            }
            // A yes was given to a value; a different value needs a new one.
            r.confirmed = prev.confirmed && prev.value == r.value;
        }
        next.append(r);
    }
    // What the user set and the suggestion no longer has: still theirs.
    for (const Row &prev : std::as_const(m_rows)) {
        if (!prev.edited)
            continue;
        bool kept = false;
        for (const Row &r : std::as_const(next))
            kept = kept || r.key == prev.key;
        if (kept)
            continue;
        Row r = prev;
        r.suggested.clear();
        r.reason = u"Not part of this suggestion; kept because you set it."_s;
        next.append(r);
    }
    m_rows = next;
}

bool SofiaParamSet::setValue(const QString &key, const QString &value)
{
    const int i = indexOf(key);
    if (i < 0)
        return false;
    Row &r = m_rows[i];
    const QString v = value.trimmed();
    r.value = v;
    // A key kept only because the user set it stays edited whatever it holds.
    r.edited = r.suggested.isEmpty() || v != r.suggested;
    if (r.needsConfirmation)
        r.confirmed = !v.isEmpty();
    return true;
}

bool SofiaParamSet::setConfirmed(const QString &key, bool confirmed)
{
    const int i = indexOf(key);
    if (i < 0 || !m_rows[i].needsConfirmation)
        return false;
    m_rows[i].confirmed = confirmed;
    return true;
}

bool SofiaParamSet::acceptsKey(const QString &key) const
{
    static const QRegularExpression name(u"^[A-Za-z]+\\.[A-Za-z0-9]+$"_s);
    const QString k = key.trimmed();
    return name.match(k).hasMatch() && (m_defaults.isEmpty() || m_defaults.contains(k));
}

bool SofiaParamSet::addUserParam(const QString &key, const QString &value)
{
    const QString k = key.trimmed();
    if (!acceptsKey(k) || indexOf(k) >= 0)
        return false;
    Row r;
    r.key = k;
    r.value = value.trimmed();
    r.defaultKnown = m_defaults.contains(k);
    r.sofiaDefault = m_defaults.value(k);
    r.hasDefault = !r.sofiaDefault.isEmpty();
    r.reason = u"Added by you; the backend checks the value when the run starts."_s;
    r.edited = true;   // suggested is empty: "set by you", kept over a new suggestion
    r.needsConfirmation = sofiaNeedsConfirmation(k);
    r.confirmed = r.needsConfirmation && !r.value.isEmpty();
    m_rows.append(r);
    return true;
}

void SofiaParamSet::resetToSuggested()
{
    QVector<Row> next;
    for (Row r : std::as_const(m_rows)) {
        if (r.suggested.isEmpty())
            continue;   // only the user had it
        if (r.edited) {
            r.value = r.suggested;
            r.edited = false;
            r.confirmed = false;   // back to a proposal
        }
        next.append(r);
    }
    m_rows = next;
}

bool SofiaParamSet::differsFromDefault(const Row &r)
{
    if (r.value.isEmpty())
        return false;
    if (!r.hasDefault || !r.defaultKnown)
        return true;
    // The suggestion: the service compared it as SoFiA reads it. A value the
    // user typed: as text, the best the client can do without SoFiA's parser.
    if (!r.edited)
        return r.suggestedChanged;
    return r.value.simplified() != r.sofiaDefault.simplified();
}

QString SofiaParamSet::rowStatus(const Row &r)
{
    if (r.needsConfirmation && !r.confirmed && !r.value.isEmpty())
        return u"proposal: tick to use"_s;
    if (r.value.isEmpty())
        return u"blank: SoFiA's default"_s;
    if (r.suggested.isEmpty())
        return u"set by you"_s;
    if (r.edited)
        return u"edited (suggested %1)"_s.arg(r.suggested);
    if (!r.hasDefault)
        return u"suggested"_s;
    return r.suggestedChanged ? u"changed from default"_s : u"SoFiA default"_s;
}

int SofiaParamSet::indexOf(const QString &key) const
{
    for (int i = 0; i < m_rows.size(); ++i)
        if (m_rows[i].key == key)
            return i;
    return -1;
}

bool SofiaParamSet::anyEdited() const
{
    for (const Row &r : m_rows)
        if (r.edited)
            return true;
    return false;
}

bool SofiaParamSet::hasUnconfirmed() const
{
    for (const Row &r : m_rows)
        if (r.needsConfirmation && !r.confirmed && !r.value.isEmpty())
            return true;
    return false;
}

QJsonObject SofiaParamSet::runParams() const
{
    QJsonObject out;
    for (const Row &r : m_rows) {
        if (r.value.isEmpty() || (r.needsConfirmation && !r.confirmed))
            continue;
        out.insert(r.key, r.value);
    }
    return out;
}

QVariantMap SofiaParamSet::provenance() const
{
    QVariantMap out;
    for (const Row &r : m_rows) {
        if (r.value.isEmpty())
            continue;
        QString how;
        if (r.needsConfirmation && !r.confirmed)
            how = u"proposal declined, not sent"_s;
        else if (r.suggested.isEmpty())
            how = u"set by you"_s;
        else if (r.edited)
            how = u"edited; suggested %1"_s.arg(r.suggested);
        else if (r.needsConfirmation)
            how = u"suggested, confirmed by you"_s;
        else
            how = u"suggested"_s;
        out.insert(r.key, u"%1 (%2)"_s.arg(r.value, how));
    }
    return out;
}

ReplayStep sofiaSourceFindStep(const QString &datasetId, const QJsonObject &params)
{
    ReplayStep step;
    step.route = u"/v1/tasks/sourcefind"_s;
    step.request = QVariantMap{
        { u"dataset_id"_s, datasetId },
        { u"finder"_s, u"sofia2"_s },
        { u"params"_s, params.toVariantMap() },
    };
    return step;
}

} // namespace visivo
