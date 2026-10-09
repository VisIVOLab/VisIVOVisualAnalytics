#include "TestBackendContract.h"

#include "BackendHeaders.h"

#include <QByteArray>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QTest>

using namespace Qt::StringLiterals;

// VISIVO_REPO_ROOT is injected by tests/CMakeLists.txt so the test can locate
// the manifest regardless of the (in-tree or standalone) build layout.
#ifndef VISIVO_REPO_ROOT
#    define VISIVO_REPO_ROOT "."
#endif

void TestBackendContract::constantsAreWellFormed()
{
    QSet<QByteArray> seen;
    for (const char *name : VisivoHeaders::kAll) {
        const QByteArray h(name);
        QVERIFY2(!h.isEmpty(), "empty header constant");
        QVERIFY2(h.startsWith("X-"), qPrintable(u"header does not start with X-: "_s
                                                + QString::fromUtf8(h)));
        QVERIFY2(!seen.contains(h), qPrintable(u"duplicate header constant: "_s
                                               + QString::fromUtf8(h)));
        seen.insert(h);
    }
    QCOMPARE(int(seen.size()), int(VisivoHeaders::kAll.size()));
}

void TestBackendContract::constantsMatchManifest()
{
    const QString manifestPath =
            QString::fromUtf8(VISIVO_REPO_ROOT)
            + u"/backend/tests/contract/visivo_headers.json"_s;
    QFile f(manifestPath);
    QVERIFY2(f.open(QIODevice::ReadOnly),
             qPrintable(u"cannot open contract manifest: "_s + manifestPath));
    QJsonParseError perr{};
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &perr);
    QVERIFY2(perr.error == QJsonParseError::NoError, qPrintable(perr.errorString()));

    QSet<QByteArray> manifest;
    for (const QJsonValue v : doc.object().value(u"headers"_s).toArray()) {
        manifest.insert(v.toString().toUtf8());
    }
    QVERIFY2(!manifest.isEmpty(), "manifest has no headers");

    QSet<QByteArray> cpp;
    for (const char *name : VisivoHeaders::kAll) {
        cpp.insert(QByteArray(name));
    }

    const QSet<QByteArray> missingInCpp = manifest - cpp;
    const QSet<QByteArray> extraInCpp = cpp - manifest;
    const auto join = [](const QSet<QByteArray> &s) {
        QStringList out;
        for (const QByteArray &b : s) out << QString::fromUtf8(b);
        return out.join(u", "_s);
    };
    QVERIFY2(missingInCpp.isEmpty(),
             qPrintable(u"headers in manifest but not in BackendHeaders.h (regenerate: "
                        "backend/.venv/bin/python backend/scripts/gen_contract.py): "_s
                        + join(missingInCpp)));
    QVERIFY2(extraInCpp.isEmpty(),
             qPrintable(u"headers in BackendHeaders.h but not in manifest: "_s
                        + join(extraInCpp)));
}
