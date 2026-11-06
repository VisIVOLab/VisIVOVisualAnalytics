// test_sofia_suggest.cpp
// The SoFiA-2 suggestion as the dialog uses it: the service's answer read into
// rows, the user's edits kept over a new suggestion (the user's choice wins),
// a proposed sub-cube sent only after a yes, and the request and replay field
// names pinned to the service's own models, so a renamed field fails here
// before it fails a run.

#include "TestSofiaSuggest.h"

#include "SofiaSuggest.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <QTest>

using namespace Qt::StringLiterals;
using visivo::SofiaParamSet;
using visivo::SofiaSuggestedParam;

#ifndef VISIVO_REPO_ROOT
#    define VISIVO_REPO_ROOT "."
#endif

namespace {

/// The HCG 44 answer of docs/backend-api/tasks-finders.md, abridged.
QJsonObject hcg44Answer()
{
    return QJsonDocument::fromJson(R"({
      "valid": true, "error": "", "dataset_id": "d1", "sofia_version": "2.7.0",
      "data_type": { "value": "emission", "deduced": true, "confidence": "high", "ambiguous": false,
        "reason": "Signal is positive.", "options": ["emission", "absorption", "continuum"] },
      "line": { "value": "hi", "deduced": true, "ambiguous": true, "confidence": "",
        "reason": "No rest frequency in the header; assuming extragalactic HI, please confirm.",
        "options": ["hi", "galactic"] },
      "parameters": [
        { "key": "scfind.kernelsXY", "value": "0, 5, 10", "reason": "Beam is 5.2 px.",
          "sofia_default": "0, 3, 6", "changed": true },
        { "key": "scfind.threshold", "value": "4.0", "reason": "With reliability.",
          "sofia_default": "5.0", "changed": true },
        { "key": "reliability.threshold", "value": "0.9", "reason": "SoFiA default.",
          "sofia_default": "0.9", "changed": false } ],
      "params": { "scfind.kernelsXY": "0, 5, 10", "scfind.threshold": "4.0", "reliability.threshold": "0.9" },
      "sofia_defaults": { "dilation.enable": "false", "input.region": "", "scfind.threshold": "5.0" },
      "measurements": { "shape": [158, 700, 700], "sigma": 0.0002246, "bunit": "JY/BEAM",
        "noise_side": "negative", "b_px": 5.202, "omega_b_px": 30.66, "channel_kms": 8.286,
        "rho": 0.7405, "l_z": 2, "r_chan": 1.078, "r_sky": 1.043, "f_nan": 0.0, "n_res": 1262000.0 },
      "warnings": [ { "code": "reliability_caveat", "message": "Few resolution elements." } ] })")
            .object();
}

SofiaSuggestedParam param(const QString &key, const QString &value, const QString &def = {},
                          bool changed = true)
{
    SofiaSuggestedParam p;
    p.key = key;
    p.value = value;
    p.reason = u"because"_s;
    p.hasDefault = !def.isEmpty();
    p.sofiaDefault = def;
    p.changed = changed;
    return p;
}

/// The fields of a pydantic model `class <name>(BaseModel):` in @p relPath:
/// one `name: type` per line at four spaces of indent, docstrings left out,
/// up to the first line back at column 0.
QSet<QString> modelFields(const QString &relPath, const QString &className)
{
    QFile f(QString::fromUtf8(VISIVO_REPO_ROOT) + u"/"_s + relPath);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    const QString src = QString::fromUtf8(f.readAll());
    const qsizetype start = src.indexOf(u"class %1(BaseModel):"_s.arg(className));
    if (start < 0)
        return {};
    static const QRegularExpression nextTopLevel(u"\\n(?=\\S)"_s);
    const QRegularExpressionMatch end = nextTopLevel.match(src, start + 1);
    QString body = src.mid(start, end.hasMatch() ? end.capturedStart() - start : -1);
    static const QRegularExpression docstring(u"\"\"\"[\\s\\S]*?\"\"\""_s);
    body.remove(docstring);
    QSet<QString> fields;
    static const QRegularExpression field(u"^    (\\w+): \\w"_s, QRegularExpression::MultilineOption);
    for (auto it = field.globalMatch(body); it.hasNext();)
        fields.insert(it.next().captured(1));
    return fields;
}

QSet<QString> keySet(const QStringList &keys)
{
    return QSet<QString>(keys.begin(), keys.end());
}

} // namespace

void TestSofiaSuggest::parse_readsTheDocumentedAnswer()
{
    const visivo::SofiaSuggestion s = visivo::parseSofiaSuggestion(hcg44Answer());
    QVERIFY(s.valid);
    QCOMPARE(s.sofiaVersion, u"2.7.0"_s);
    QVERIFY(s.dataType.present);
    QCOMPARE(s.dataType.value, u"emission"_s);
    QCOMPARE(s.dataType.confidence, u"high"_s);
    QVERIFY(!s.dataType.ambiguous);
    QCOMPARE(s.dataType.options.size(), 3);
    QVERIFY(s.line.ambiguous);
    QCOMPARE(s.line.value, u"hi"_s);
    QCOMPARE(s.parameters.size(), 3);
    QCOMPARE(s.parameters[0].key, u"scfind.kernelsXY"_s);
    QCOMPARE(s.parameters[0].value, u"0, 5, 10"_s);
    QCOMPARE(s.parameters[0].sofiaDefault, u"0, 3, 6"_s);
    QVERIFY(s.parameters[0].hasDefault);
    QVERIFY(s.parameters[0].changed);
    QVERIFY(!s.parameters[2].changed);
    QCOMPARE(s.warnings.size(), 1);
    QCOMPARE(s.warnings[0].first, u"reliability_caveat"_s);
    QCOMPARE(s.measurements.value(u"l_z"_s).toInt(), 2);
    QCOMPARE(s.sofiaDefaults.value(u"dilation.enable"_s), u"false"_s);
    QVERIFY(s.sofiaDefaults.contains(u"input.region"_s));
}

void TestSofiaSuggest::parse_realAnswerRunsAsTheServiceMapsIt()
{
    // The suggester's own answer on HCG 44 (hgc44_3axes.fits), saved from the
    // backend: untouched, the dialog must send exactly the service's `params`.
    QFile f(QString::fromUtf8(VISIVO_REPO_ROOT) + u"/tests/data/sofia_suggest_hcg44.json"_s);
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QJsonObject body = QJsonDocument::fromJson(f.readAll()).object();
    const visivo::SofiaSuggestion s = visivo::parseSofiaSuggestion(body);
    QVERIFY(s.valid);
    QCOMPARE(s.parameters.size(), 18);
    // The flux goal left v1: the suggestion never sets dilation.
    for (const auto &p : s.parameters)
        QVERIFY2(!p.key.startsWith(u"dilation."_s), qPrintable(p.key));
    QVERIFY(!s.dataType.ambiguous);
    SofiaParamSet set;
    set.applySuggestion(s.parameters);
    QCOMPARE(set.runParams(), body.value(u"params"_s).toObject());
    QVERIFY(!set.hasUnconfirmed());
    const QString line = visivo::sofiaMeasurementLine(s.measurements);
    QVERIFY2(line.contains(u"beam 5.2 px"_s), qPrintable(line));
    QVERIFY2(line.contains(u"158×700×700"_s), qPrintable(line));
    for (const auto &row : set.rows())
        QVERIFY2(!SofiaParamSet::rowStatus(row).isEmpty(), qPrintable(row.key));
}

void TestSofiaSuggest::parse_handlesMissingDefaultsAndOptions()
{
    QJsonObject body = hcg44Answer();
    QJsonArray params;
    params.append(QJsonObject{ { u"key"_s, u"input.region"_s }, { u"value"_s, u"10, 99, 10, 99, 0, 157"_s },
                               { u"reason"_s, u"memory"_s }, { u"sofia_default"_s, QJsonValue() },
                               { u"changed"_s, true } });
    params.append(QJsonObject{ { u"value"_s, u"no key"_s } });   // dropped
    body[u"parameters"_s] = params;
    // A value the options do not list is still offered, first.
    QJsonObject line = body.value(u"line"_s).toObject();
    line[u"options"_s] = QJsonArray{ u"galactic"_s };
    body[u"line"_s] = line;
    body.remove(u"data_type"_s);

    const visivo::SofiaSuggestion s = visivo::parseSofiaSuggestion(body);
    QCOMPARE(s.parameters.size(), 1);
    QVERIFY(!s.parameters[0].hasDefault);
    QVERIFY(!s.dataType.present);
    QCOMPARE(s.line.options, (QStringList{ u"hi"_s, u"galactic"_s }));
}

void TestSofiaSuggest::parse_noParametersIsNotASuggestion()
{
    QJsonObject body = hcg44Answer();
    body[u"parameters"_s] = QJsonArray{};
    const visivo::SofiaSuggestion s = visivo::parseSofiaSuggestion(body);
    QVERIFY(!s.valid);
    QVERIFY(!s.error.isEmpty());
}

void TestSofiaSuggest::measurementLine_isCompactAndSkipsWhatIsMissing()
{
    const visivo::SofiaSuggestion s = visivo::parseSofiaSuggestion(hcg44Answer());
    const QString line = visivo::sofiaMeasurementLine(s.measurements);
    QVERIFY2(line.contains(u"σ 0.000225 JY/BEAM"_s), qPrintable(line));
    QVERIFY2(line.contains(u"beam 5.2 px"_s), qPrintable(line));
    QVERIFY2(line.contains(u"Δv 8.29 km/s"_s), qPrintable(line));
    QVERIFY2(line.contains(u"ρ 0.74"_s), qPrintable(line));
    QVERIFY2(line.contains(u"N_res 1.26e6"_s), qPrintable(line));
    QVERIFY2(line.contains(u"158×700×700"_s), qPrintable(line));
    QVERIFY2(!line.contains(u"blank"_s), qPrintable(line));   // f_nan is 0

    // No beam, no channel width (a FREQ axis without RESTFRQ): said, not hidden.
    QVariantMap bare{ { u"sigma"_s, 0.002 }, { u"b_px"_s, QVariant() }, { u"channel_kms"_s, QVariant() } };
    const QString b = visivo::sofiaMeasurementLine(bare);
    QVERIFY2(b.contains(u"no beam in the header"_s), qPrintable(b));
    QVERIFY2(b.contains(u"channel width unknown"_s), qPrintable(b));
    QVERIFY2(!b.contains(u"ρ"_s), qPrintable(b));
}

void TestSofiaSuggest::edits_surviveANewSuggestion()
{
    SofiaParamSet set;
    set.applySuggestion({ param(u"scfind.threshold"_s, u"4.0"_s, u"5.0"_s),
                          param(u"linker.minSizeZ"_s, u"5"_s, u"5"_s, false) });
    QVERIFY(set.setValue(u"scfind.threshold"_s, u" 4.5 "_s));
    QVERIFY(set.anyEdited());
    QCOMPARE(set.runParams().value(u"scfind.threshold"_s).toString(), u"4.5"_s);

    // Another data type comes back with other values: the user's 4.5 stays, the
    // untouched key follows the suggestion.
    set.applySuggestion({ param(u"scfind.threshold"_s, u"5.0"_s, u"5.0"_s, false),
                          param(u"linker.minSizeZ"_s, u"7"_s, u"5"_s) });
    const auto &rows = set.rows();
    QCOMPARE(rows.size(), 2);
    QCOMPARE(rows[0].value, u"4.5"_s);
    QCOMPARE(rows[0].suggested, u"5.0"_s);
    QVERIFY(rows[0].edited);
    QCOMPARE(rows[1].value, u"7"_s);
    QVERIFY(!rows[1].edited);
    QCOMPARE(set.runParams(), (QJsonObject{ { u"scfind.threshold"_s, u"4.5"_s },
                                           { u"linker.minSizeZ"_s, u"7"_s } }));
    QCOMPARE(set.provenance().value(u"scfind.threshold"_s).toString(), u"4.5 (edited; suggested 5.0)"_s);
    QCOMPARE(set.provenance().value(u"linker.minSizeZ"_s).toString(), u"7 (suggested)"_s);

    // A new suggestion that happens to agree with the edit absorbs it.
    set.applySuggestion({ param(u"scfind.threshold"_s, u"4.5"_s, u"5.0"_s) });
    QVERIFY(!set.rows()[0].edited);
}

void TestSofiaSuggest::edits_typingTheSuggestionBackClearsTheEdit()
{
    SofiaParamSet set;
    set.applySuggestion({ param(u"scfind.kernelsZ"_s, u"0, 3, 7, 15"_s, u"0, 3, 7, 15"_s, false) });
    set.setValue(u"scfind.kernelsZ"_s, u"0, 3, 7"_s);
    QVERIFY(set.rows()[0].edited);
    set.setValue(u"scfind.kernelsZ"_s, u"0, 3, 7, 15"_s);
    QVERIFY(!set.rows()[0].edited);
    QVERIFY(!set.anyEdited());
    QVERIFY(!set.setValue(u"no.such"_s, u"1"_s));

    // A blank leaves SoFiA's default: the key is not sent.
    set.setValue(u"scfind.kernelsZ"_s, u""_s);
    QVERIFY(set.runParams().isEmpty());
}

void TestSofiaSuggest::edits_keysTheSuggestionDropsGoUnlessTheUserSetThem()
{
    SofiaParamSet set;
    set.applySuggestion({ param(u"dilation.enable"_s, u"true"_s, u"false"_s),
                          param(u"dilation.iterationsXY"_s, u"5"_s, u"10"_s),
                          param(u"scfind.threshold"_s, u"4.0"_s, u"5.0"_s) });
    set.setValue(u"dilation.iterationsXY"_s, u"3"_s);
    // A new suggestion without the dilation keys.
    set.applySuggestion({ param(u"scfind.threshold"_s, u"4.0"_s, u"5.0"_s) });
    QCOMPARE(set.rows().size(), 2);
    QCOMPARE(set.indexOf(u"dilation.enable"_s), -1);
    const int kept = set.indexOf(u"dilation.iterationsXY"_s);
    QVERIFY(kept >= 0);
    QVERIFY(set.rows()[kept].suggested.isEmpty());
    QVERIFY(set.rows()[kept].edited);
    QCOMPARE(SofiaParamSet::rowStatus(set.rows()[kept]), u"set by you"_s);
    QCOMPARE(set.provenance().value(u"dilation.iterationsXY"_s).toString(), u"3 (set by you)"_s);
    QCOMPARE(set.runParams().value(u"dilation.iterationsXY"_s).toString(), u"3"_s);
}

void TestSofiaSuggest::reset_putsEveryEditBack()
{
    SofiaParamSet set;
    set.applySuggestion({ param(u"dilation.iterationsXY"_s, u"5"_s, u"10"_s),
                          param(u"scfind.threshold"_s, u"4.0"_s, u"5.0"_s) });
    set.setValue(u"dilation.iterationsXY"_s, u"3"_s);
    set.setValue(u"scfind.threshold"_s, u"3.5"_s);
    set.applySuggestion({ param(u"scfind.threshold"_s, u"4.0"_s, u"5.0"_s) });
    set.resetToSuggested();
    QCOMPARE(set.rows().size(), 1);
    QCOMPARE(set.rows()[0].value, u"4.0"_s);
    QVERIFY(!set.anyEdited());
}

void TestSofiaSuggest::region_isSentOnlyAfterAYes()
{
    const QString region = u"100, 599, 100, 599, 0, 157"_s;
    SofiaParamSet set;
    set.applySuggestion({ param(u"input.region"_s, region),
                          param(u"scfind.threshold"_s, u"4.0"_s, u"5.0"_s) });
    QVERIFY(set.rows()[0].needsConfirmation);
    QVERIFY(set.hasUnconfirmed());
    QVERIFY(!set.runParams().contains(u"input.region"_s));
    QCOMPARE(SofiaParamSet::rowStatus(set.rows()[0]), u"proposal: tick to use"_s);
    QCOMPARE(set.provenance().value(u"input.region"_s).toString(),
             region + u" (proposal declined, not sent)"_s);

    QVERIFY(set.setConfirmed(u"input.region"_s, true));
    QVERIFY(!set.hasUnconfirmed());
    QCOMPARE(set.runParams().value(u"input.region"_s).toString(), region);
    QCOMPARE(set.provenance().value(u"input.region"_s).toString(),
             region + u" (suggested, confirmed by you)"_s);
    QVERIFY(!set.setConfirmed(u"scfind.threshold"_s, true));   // not a proposal

    // The same proposal again keeps the yes; a different one needs a new one.
    set.applySuggestion({ param(u"input.region"_s, region) });
    QVERIFY(set.rows()[0].confirmed);
    set.applySuggestion({ param(u"input.region"_s, u"0, 499, 0, 499, 0, 157"_s) });
    QVERIFY(!set.rows()[0].confirmed);

    // Typing a region is the user's own: no tick needed.
    set.setValue(u"input.region"_s, u"0, 99, 0, 99, 0, 157"_s);
    QVERIFY(set.rows()[0].confirmed);
    QCOMPARE(set.runParams().value(u"input.region"_s).toString(), u"0, 99, 0, 99, 0, 157"_s);

    // Reset: back to the proposal, and to asking.
    set.resetToSuggested();
    QVERIFY(!set.rows()[0].confirmed);
    QVERIFY(set.hasUnconfirmed());
}

void TestSofiaSuggest::status_saysHowEachValueCameToBe()
{
    SofiaParamSet set;
    set.applySuggestion({ param(u"scfind.threshold"_s, u"4.0"_s, u"5.0"_s, true),
                          param(u"reliability.threshold"_s, u"0.9"_s, u"0.9"_s, false),
                          param(u"scfind.replacement"_s, u"2.0"_s, u"2"_s, false) });
    const auto &rows = set.rows();
    QCOMPARE(SofiaParamSet::rowStatus(rows[0]), u"changed from default"_s);
    QVERIFY(SofiaParamSet::differsFromDefault(rows[0]));
    QCOMPARE(SofiaParamSet::rowStatus(rows[1]), u"SoFiA default"_s);
    QVERIFY(!SofiaParamSet::differsFromDefault(rows[1]));
    // "2.0" against "2": the service said they are the same number.
    QVERIFY(!SofiaParamSet::differsFromDefault(rows[2]));

    set.setValue(u"reliability.threshold"_s, u"0.8"_s);
    QCOMPARE(SofiaParamSet::rowStatus(set.rows()[1]), u"edited (suggested 0.9)"_s);
    QVERIFY(SofiaParamSet::differsFromDefault(set.rows()[1]));
    set.setValue(u"reliability.threshold"_s, u""_s);
    QCOMPARE(SofiaParamSet::rowStatus(set.rows()[1]), u"blank: SoFiA's default"_s);
}

void TestSofiaSuggest::request_usesTheServiceRequestModelsFieldNames()
{
    const QSet<QString> service = modelFields(u"backend/app/schemas.py"_s, u"Sofia2SuggestRequest"_s);
    QVERIFY2(!service.isEmpty(), "could not read Sofia2SuggestRequest from backend/app/schemas.py");
    const QJsonObject deduce = visivo::sofiaSuggestRequest(QString(), QString());
    QCOMPARE(keySet(deduce.keys()), service);   // no goal: it left v1
    QVERIFY(deduce.value(u"data_type"_s).isNull());
    QVERIFY(deduce.value(u"line"_s).isNull());

    const QJsonObject chosen = visivo::sofiaSuggestRequest(u"absorption"_s, u"galactic"_s);
    QCOMPARE(chosen.value(u"data_type"_s).toString(), u"absorption"_s);
    QCOMPARE(chosen.value(u"line"_s).toString(), u"galactic"_s);
}

void TestSofiaSuggest::replay_usesTheTaskRouteRequestModelsFieldNames()
{
    const QSet<QString> service =
            modelFields(u"backend/app/routers/tasks.py"_s, u"SourceFindTaskRequest"_s);
    QVERIFY2(!service.isEmpty(), "could not read SourceFindTaskRequest from the router");
    const QJsonObject params{ { u"scfind.threshold"_s, u"4.0"_s }, { u"scfind.kernelsXY"_s, u"0, 5, 10"_s } };
    const ReplayStep step = visivo::sofiaSourceFindStep(u"d1"_s, params);
    QCOMPARE(step.route, u"/v1/tasks/sourcefind"_s);
    QCOMPARE(keySet(step.request.keys()), service);
    QCOMPARE(step.request.value(u"finder"_s).toString(), u"sofia2"_s);
    const QVariantMap sent = step.request.value(u"params"_s).toMap();
    QCOMPARE(sent.value(u"scfind.kernelsXY"_s).toString(), u"0, 5, 10"_s);
    QCOMPARE(sent.size(), 2);
}

void TestSofiaSuggest::ask_failedRequestGoesBackToTheAnsweredOne()
{
    visivo::SofiaAskState ask;
    // A first request that fails before any answer: nothing to go back to.
    ask.pending.line = u"hi"_s;
    ask.failed();
    QCOMPARE(ask.pending.line, u"hi"_s);

    // Answered: emission deduced.
    ask.succeeded();
    // The user picks absorption; the backend is busy.
    ask.pending.dataType = u"absorption"_s;
    ask.failed();
    // The table still holds the emission suggestion: so does the ask.
    QCOMPARE(ask.pending.dataType, QString());
    QCOMPARE(ask.pending.line, u"hi"_s);

    // A choice that was answered stays asked.
    ask.pending.line = u"galactic"_s;
    ask.succeeded();
    ask.pending.dataType = u"continuum"_s;
    ask.failed();
    QCOMPARE(ask.pending.line, u"galactic"_s);
    QCOMPARE(ask.pending.dataType, QString());
}

void TestSofiaSuggest::errorDetail_readsFastApiBodies()
{
    QCOMPARE(visivo::backendErrorDetail(u"{\"detail\": \"Stokes Q cube: polarisation\"}"_s),
             u"Stokes Q cube: polarisation"_s);
    QCOMPARE(visivo::backendErrorDetail(
                     u"{\"detail\": [{\"loc\": [\"body\", \"line\"], \"msg\": \"Input should be 'hi' or 'galactic'\"}]}"_s),
             u"line: Input should be 'hi' or 'galactic'"_s);
    QCOMPARE(visivo::backendErrorDetail(u"  Connection refused  "_s), u"Connection refused"_s);
    QCOMPARE(visivo::backendErrorDetail(u"{\"valid\": false, \"error\": \"busy\"}"_s), u"busy"_s);
}

void TestSofiaSuggest::display_maskColumnsAndRunLabels()
{
    // The SoFiA mask, wherever the backend wrote it: opened in MIP.
    QVERIFY(visivo::isSofiaMaskPath(u"/ws/exports/sourcefind/abc123/sofia_out_mask.fits"_s));
    QVERIFY(visivo::isSofiaMaskPath(u"C:\\ws\\sourcefind\\t\\sofia_out_mask.fits"_s));
    QVERIFY(!visivo::isSofiaMaskPath(u"/data/hgc44_3axes.fits"_s));
    QVERIFY(!visivo::isSofiaMaskPath(u"/data/not_sofia_out_mask.fits"_s));

    // What a row is comes first; the catalogue used to open on `dec`.
    QCOMPARE(visivo::sofiaCatalogueColumns({ u"x"_s, u"dec"_s, u"name"_s, u"ra"_s, u"id"_s, u"dec"_s }),
             (QStringList{ u"id"_s, u"name"_s, u"dec"_s, u"ra"_s, u"x"_s }));
    QCOMPARE(visivo::sofiaCatalogueColumns({ u"z"_s, u"f_sum"_s }), (QStringList{ u"f_sum"_s, u"z"_s }));

    // Two runs of the same parameters differ by their time; edits show.
    QCOMPARE(visivo::sofiaRunLabel(12, 0, u"14:32:05"_s), u"SoFiA-2 · 12 sources · 14:32:05"_s);
    QCOMPARE(visivo::sofiaRunLabel(1, 2, u"14:40:00"_s), u"SoFiA-2 · 1 source · 2 edited · 14:40:00"_s);
    QCOMPARE(visivo::sofiaRunLabel(0, 0, QString()), u"SoFiA-2 · 0 sources"_s);
}

void TestSofiaSuggest::emptyResult_isAResultNotAFailure()
{
    // Exit 8: completed, no sources, SoFiA's ERROR line in the summary.
    const QJsonObject result = QJsonDocument::fromJson(R"({
        "valid": true, "n_sources": 0, "sources": [], "mask_path": "", "catalog_path": "",
        "exit_code": 8,
        "summary": { "n_positive": null, "n_negative": null, "rho": 0.02, "n_after_filter": null,
                     "warnings": ["Only 3 negative detections found."],
                     "errors": ["No sources left after filtering. Terminating pipeline."],
                     "note": "No sources left after filtering." } })")
                                       .object();
    QCOMPARE(visivo::sofiaExitCode(result), 8);
    QCOMPARE(visivo::sofiaRunHeadline(0, 8), u"No sources left after filtering."_s);
    QCOMPARE(visivo::sofiaRunHeadline(12, 0), u"12 sources found."_s);
    QCOMPARE(visivo::sofiaRunHeadline(0, 0), u"0 sources found."_s);
    QCOMPARE(visivo::sofiaExitCode(QJsonObject{}), -1);   // an older backend

    const visivo::SofiaRunSummary s = visivo::parseSofiaRunSummary(result.value(u"summary"_s).toObject());
    QCOMPARE(s.errors.size(), 1);
    const QString text = visivo::sofiaRunSummaryText(s);
    // The note says it once; the matching ERROR line is not repeated.
    QCOMPARE(text.count(u"No sources left"_s), 1);
    QVERIFY2(text.contains(u"SoFiA warned: Only 3 negative"_s), qPrintable(text));

    // Another error is shown.
    visivo::SofiaRunSummary other = s;
    other.errors = { u"Failed to read FITS header."_s };
    QVERIFY(visivo::sofiaRunSummaryText(other).contains(u"SoFiA reported: Failed to read FITS header."_s));
    // First sentences without a full stop do not run together.
    visivo::SofiaRunSummary two = s;
    two.warnings = { u"FITS header keyword of CELLSCAL = '1/F' detected"_s,
                     u"Significant channel-to-channel noise correlation detected."_s };
    QVERIFY(visivo::sofiaRunSummaryText(two).contains(u"'1/F' detected. Significant"_s));
}

void TestSofiaSuggest::addedParams_areTheUsersAndSurvive()
{
    SofiaParamSet set;
    set.applySuggestion({ param(u"scfind.threshold"_s, u"4.0"_s, u"5.0"_s) });
    QVERIFY(set.addUserParam(u"dilation.enable"_s, u"true"_s));
    QVERIFY(!set.addUserParam(u"dilation.enable"_s, u"false"_s));   // already a row
    QVERIFY(!set.addUserParam(u"dilation"_s, u"true"_s));          // not section.key
    QVERIFY(!set.addUserParam(u"input.data = /etc/passwd"_s, u"x"_s));
    const int i = set.indexOf(u"dilation.enable"_s);
    QCOMPARE(SofiaParamSet::rowStatus(set.rows()[i]), u"set by you"_s);
    QCOMPARE(set.runParams().value(u"dilation.enable"_s).toString(), u"true"_s);
    // A new suggestion keeps it; a reset drops it.
    set.applySuggestion({ param(u"scfind.threshold"_s, u"5.0"_s, u"5.0"_s, false) });
    QVERIFY(set.indexOf(u"dilation.enable"_s) >= 0);
    set.resetToSuggested();
    QCOMPARE(set.indexOf(u"dilation.enable"_s), -1);
}

void TestSofiaSuggest::addedParams_takeTheBackendsDefaultsAndNames()
{
    const visivo::SofiaSuggestion s = visivo::parseSofiaSuggestion(hcg44Answer());
    SofiaParamSet set;
    set.setSofiaDefaults(s.sofiaDefaults);
    set.applySuggestion(s.parameters);
    // A key the backend accepts gets SoFiA's real default, not "none".
    QVERIFY(set.addUserParam(u"dilation.enable"_s, u"true"_s));
    const SofiaParamSet::Row &dil = set.rows()[set.indexOf(u"dilation.enable"_s)];
    QVERIFY(dil.defaultKnown && dil.hasDefault);
    QCOMPARE(dil.sofiaDefault, u"false"_s);
    QVERIFY(dil.reason.contains(u"backend checks"_s));
    // One it has no default for: known to have none.
    QVERIFY(set.addUserParam(u"input.region"_s, u"0, 9, 0, 9, 0, 9"_s));
    const SofiaParamSet::Row &region = set.rows()[set.indexOf(u"input.region"_s)];
    QVERIFY(region.defaultKnown && !region.hasDefault);
    // A name it does not accept is refused when it is added.
    QVERIFY(!set.acceptsKey(u"foo.bar"_s));
    QVERIFY(!set.addUserParam(u"foo.bar"_s, u"1"_s));
    QVERIFY(!set.addUserParam(u"output.filename"_s, u"x"_s));

    // An older backend sends no defaults: every section.key is taken, and its
    // default is unknown rather than "none".
    SofiaParamSet old;
    QVERIFY(old.acceptsKey(u"foo.bar"_s));
    QVERIFY(old.addUserParam(u"dilation.enable"_s, u"true"_s));
    QVERIFY(!old.rows()[old.indexOf(u"dilation.enable"_s)].defaultKnown);
}

void TestSofiaSuggest::failureText_saysWhetherSofiaRan()
{
    // A task that SoFiA-2 itself failed: as the backend words it.
    const QString ran = u"SoFiA-2 failed (exit 7): Negative flux threshold encountered."_s;
    QCOMPARE(visivo::sofiaFailureText(ran, 0), ran);
    // Refused at creation: no task, SoFiA-2 never started.
    QCOMPARE(visivo::sofiaFailureText(u"parameter 'foo.bar' is not a SoFiA-2 2.7 parameter"_s, 422),
             u"SoFiA-2 did not run.\nparameter 'foo.bar' is not a SoFiA-2 2.7 parameter"_s);
    // A task lost after it started may still be running: neither said.
    const QString lost = visivo::sofiaFailureText(
            u"lost track of task t1 on the backend (timeout); it may still be running"_s, 0);
    QVERIFY(lost.startsWith(u"The run did not finish."_s));
    QVERIFY(!lost.contains(u"did not run"_s));
}

void TestSofiaSuggest::refusal_readsTheBodyNotQtsErrorString()
{
    // What performPost hands back for a 422: Qt's error string, and the body.
    const QString qtError = u"Error transferring http://127.0.0.1:8000/v1/sofia2/suggest/d1 - "
                            u"server replied: Unprocessable Entity"_s;
    const QByteArray body = R"({"detail": "The cube is Stokes Q, not I: polarisation cubes need RM synthesis."})";
    QCOMPARE(visivo::refusalText(422, body, qtError),
             u"The cube is Stokes Q, not I: polarisation cubes need RM synthesis."_s);
    // The route an older backend lacks: the dialog keys on this exact text.
    QCOMPARE(visivo::refusalText(404, R"({"detail":"Not Found"})", qtError), u"Not Found"_s);
    // No answer at all, or a refusal without a body: Qt's words are all there is.
    QCOMPARE(visivo::refusalText(0, QByteArray(), u"Connection refused"_s), u"Connection refused"_s);
    QCOMPARE(visivo::refusalText(503, QByteArray(), qtError), qtError);
}

void TestSofiaSuggest::runSummary_readsSofiasCounts()
{
    const QJsonObject summary = QJsonDocument::fromJson(R"({
        "n_positive": 53, "n_negative": 33, "rho": 0.741, "n_after_filter": 18,
        "warnings": ["Significant channel-to-channel noise correlation detected."],
        "note": "Reliability ran on 33 negative and 53 positive detections." })")
                                        .object();
    const visivo::SofiaRunSummary s = visivo::parseSofiaRunSummary(summary);
    QVERIFY(s.present);
    QCOMPARE(s.nNegative, 33);
    QCOMPARE(s.nPositive, 53);
    QCOMPARE(s.nAfterFilter, 18);
    QVERIFY(s.hasRho);
    const QString text = visivo::sofiaRunSummaryText(s);
    QVERIFY2(text.startsWith(u"Reliability ran on 33 negative and 53 positive detections."_s), qPrintable(text));
    QVERIFY2(text.contains(u"18 source(s) left"_s), qPrintable(text));
    QVERIFY2(text.contains(u"0.741"_s), qPrintable(text));
    QVERIFY2(text.contains(u"SoFiA warned: Significant"_s), qPrintable(text));

    // Reliability off: the counts are null and there is no note.
    const visivo::SofiaRunSummary off = visivo::parseSofiaRunSummary(QJsonDocument::fromJson(
            R"({ "n_positive": null, "n_negative": null, "rho": null, "n_after_filter": 4,
                 "warnings": [], "note": "" })").object());
    QCOMPARE(off.nNegative, -1);
    QVERIFY(!off.hasRho);
    QCOMPARE(visivo::sofiaRunSummaryText(off), u"4 source(s) left after reliability and filtering."_s);

    QVERIFY(!visivo::parseSofiaRunSummary(QJsonObject{}).present);
    QVERIFY(visivo::sofiaRunSummaryText(visivo::SofiaRunSummary{}).isEmpty());
}
