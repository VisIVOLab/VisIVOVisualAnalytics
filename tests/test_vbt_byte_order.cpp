// test_vbt_byte_order.cpp
// changavbtDARK.bin.head says little-endian; its data are big-endian. The
// backend's open answer says so (byte_order_warning); the client asks, and
// remembers the answer so every later open of the file reads it the same way.

#include "TestVbtByteOrder.h"

#include "ProductRecipe.h"
#include "ProductRegistry.h"
#include "VbtByteOrder.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QTest>

using namespace Qt::StringLiterals;

namespace {

QJsonObject darkWarning(const QString &confidence = u"overwhelming"_s)
{
    // As app/vbt_byte_order.py writes it.
    return QJsonDocument::fromJson(QStringLiteral(R"({
        "declared": "little", "suggested": "big", "confidence": "%1",
        "evidence": {"implausible_declared": 0.6234, "implausible_suggested": 0.0,
                     "sampled_values": 18432, "informative_fields": 7,
                     "fields": [{"name": "MASS", "implausible_declared": 0.0,
                                 "implausible_suggested": 0.0,
                                 "first_declared": "-1.273e+24",
                                 "first_suggested": "2.45e-05"}]}
    })").arg(confidence).toUtf8()).object();
}

} // namespace

void TestVbtByteOrder::theWarningIsParsedFromTheOpenAnswer()
{
    const auto w = visivo::parseVbtByteOrderWarning(darkWarning());
    QVERIFY(w.present);
    QCOMPARE(w.declared, u"little"_s);
    QCOMPARE(w.suggested, u"big"_s);
    QCOMPARE(w.confidence, u"overwhelming"_s);
    QCOMPARE(w.implausibleDeclared, 0.6234);
    QCOMPARE(w.field, u"MASS"_s);
    QCOMPARE(w.firstDeclared, u"-1.273e+24"_s);
    QCOMPARE(w.firstSuggested, u"2.45e-05"_s);
}

void TestVbtByteOrder::noWarningOrANonsenseOneIsAbsent()
{
    QVERIFY(!visivo::parseVbtByteOrderWarning(QJsonValue()).present);
    QVERIFY(!visivo::parseVbtByteOrderWarning(QJsonValue(QJsonValue::Null)).present);
    QJsonObject same = darkWarning();
    same.insert(u"suggested"_s, u"little"_s);
    QVERIFY(!visivo::parseVbtByteOrderWarning(same).present);
    QJsonObject odd = darkWarning();
    odd.insert(u"declared"_s, u"middle"_s);
    QVERIFY(!visivo::parseVbtByteOrderWarning(odd).present);
}

void TestVbtByteOrder::theQuestionNamesBothOrdersAndTheEvidence()
{
    const auto w = visivo::parseVbtByteOrderWarning(darkWarning());
    QCOMPARE(visivo::vbtByteOrderQuestion(w),
             u"This table's header says little-endian, but its values look big-endian."_s);
    const QString evidence = visivo::vbtByteOrderEvidence(w);
    QVERIFY(evidence.contains(u"62% of the sampled values"_s));
    QVERIFY(evidence.contains(u"First MASS: -1.273e+24 as declared, 2.45e-05 as big-endian."_s));
    QVERIFY(evidence.contains(u"not changed"_s));
}

void TestVbtByteOrder::onlyOverwhelmingEvidenceDefaultsToTheSuggestedOrder()
{
    QVERIFY(visivo::vbtByteOrderDefaultsToSuggested(
            visivo::parseVbtByteOrderWarning(darkWarning())));
    const auto strong = visivo::parseVbtByteOrderWarning(darkWarning(u"strong"_s));
    QVERIFY(!visivo::vbtByteOrderDefaultsToSuggested(strong));
    QVERIFY(!visivo::vbtByteOrderDefaultsToSuggested(strong, u"little"_s));
    // Already read in the suggested order (a choice made before): that stays
    // the default when the question comes back.
    QVERIFY(visivo::vbtByteOrderDefaultsToSuggested(strong, u"big"_s));
    QVERIFY(!visivo::vbtByteOrderDefaultsToSuggested(visivo::VbtByteOrderWarning{}));
}

void TestVbtByteOrder::theChoiceIsRememberedPerBackendAndDataFile()
{
    const QString local = u"http://127.0.0.1:18900"_s;
    const QString remote = u"http://127.0.0.1:18960"_s;
    const QString head = u"/scratch/sim/changavbtDARK.bin.head"_s;
    visivo::rememberVbtByteOrder(local, head, u"big"_s);
    // The .head, the data and a field reference name one file.
    QCOMPARE(visivo::rememberedVbtByteOrder(local, u"/scratch/sim/changavbtDARK.bin"_s), u"big"_s);
    QCOMPARE(visivo::rememberedVbtByteOrder(
                     local, u"/scratch/sim/changavbtDARK.bin::vbt-field=2"_s), u"big"_s);
    QCOMPARE(visivo::rememberedVbtByteOrder(local, head), u"big"_s);
    // Another backend has its own files.
    QVERIFY(visivo::rememberedVbtByteOrder(remote, head).isEmpty());
    QVERIFY(visivo::rememberedVbtByteOrder(local, u"/scratch/sim/other.bin.head"_s).isEmpty());
    visivo::rememberVbtByteOrder(local, head, QString());
    QVERIFY(visivo::rememberedVbtByteOrder(local, head).isEmpty());
}

void TestVbtByteOrder::theRecipeRecordsTheChosenOrder()
{
    AnalysisProduct p;
    p.productId = u"moment-0"_s;
    p.kind = u"moment"_s;
    p.parentKey = u"/scratch/sim/gas.bin::vbt-field=0"_s;
    const auto datasetOf = [&p] {
        return visivo::productRecipe(p, u"VisIVO"_s, u"2026-11-10T10:00:00Z"_s)
                .value(u"dataset"_s).toObject();
    };
    p.sessionId = u"sess-local"_s;
    QVERIFY(!datasetOf().contains(u"byte_order"_s));
    // A choice made in the client is not what the recipe records...
    visivo::rememberVbtByteOrder(u"http://127.0.0.1:18900"_s, u"/scratch/sim/gas.bin.head"_s,
                                 u"big"_s);
    snapshotParentByteOrder(p);
    QVERIFY(!datasetOf().contains(u"byte_order"_s));
    // ...nor an answer on another backend session...
    visivo::noteVbtReadOrder(u"sess-other"_s, u"/scratch/sim/gas.bin.head"_s, u"little"_s,
                             u"little"_s);
    snapshotParentByteOrder(p);
    QVERIFY(!datasetOf().contains(u"byte_order"_s));
    // ...the backend's answer on the product's session, when it was made, is.
    visivo::noteVbtReadOrder(u"sess-local"_s, u"/scratch/sim/gas.bin.head"_s, u"big"_s,
                             u"little"_s);
    snapshotParentByteOrder(p);
    QCOMPARE(datasetOf().value(u"byte_order"_s).toString(), u"big"_s);
    QCOMPARE(datasetOf().value(u"byte_order_declared"_s).toString(), u"little"_s);
    // A later reopen in the other order does not rewrite the product's.
    visivo::noteVbtReadOrder(u"sess-local"_s, u"/scratch/sim/gas.bin.head"_s, u"little"_s,
                             u"little"_s);
    snapshotParentByteOrder(p);
    QCOMPARE(datasetOf().value(u"byte_order"_s).toString(), u"big"_s);
    visivo::rememberVbtByteOrder(u"http://127.0.0.1:18900"_s, u"/scratch/sim/gas.bin.head"_s,
                                 QString());
}
