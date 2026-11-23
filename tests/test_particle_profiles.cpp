// test_particle_profiles.cpp
// See TestParticleProfiles.h.

#include "TestParticleProfiles.h"

#include "app/BackendClient.h"
#include "app/BackendTaskFollow.h"
#include "app/ParticleProfiles.h"

#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QtConcurrent>
#include <QtTest>

#include <cmath>
#include <functional>
#include <memory>

using namespace Qt::StringLiterals;
namespace profiles = visivo::profiles;

namespace {

struct Request
{
    QByteArray method;
    QByteArray path;
    QJsonObject body;
};

struct Reply
{
    int status = 200;
    QJsonObject body;
};

/// A backend on the test's thread, as in test_diagnostics_task.cpp: each
/// request is recorded and answered by @p route.
class MockBackend : public QObject
{
public:
    explicit MockBackend(std::function<Reply(const Request &)> route) : route(std::move(route))
    {
        server.listen(QHostAddress::LocalHost);
        QObject::connect(&server, &QTcpServer::newConnection, this, [this]() {
            while (QTcpSocket *s = server.nextPendingConnection()) {
                auto buffer = std::make_shared<QByteArray>();
                QObject::connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
                QObject::connect(s, &QTcpSocket::readyRead, this, [this, s, buffer]() {
                    *buffer += s->readAll();
                    const qsizetype end = buffer->indexOf("\r\n\r\n");
                    if (end < 0)
                        return;
                    const QByteArray head = buffer->left(end);
                    qsizetype length = 0;
                    for (const QByteArray &line : head.split('\n')) {
                        const QByteArray l = line.trimmed();
                        if (l.toLower().startsWith("content-length:"))
                            length = l.mid(15).trimmed().toLongLong();
                    }
                    if (buffer->size() < end + 4 + length)
                        return;
                    const QList<QByteArray> first = head.left(head.indexOf('\r')).split(' ');
                    Request request;
                    request.method = first.value(0);
                    request.path = first.value(1);
                    request.body = QJsonDocument::fromJson(buffer->mid(end + 4, length)).object();
                    buffer->clear();
                    requests.append(request);
                    const Reply reply = this->route(request);
                    const QByteArray payload =
                            QJsonDocument(reply.body).toJson(QJsonDocument::Compact);
                    s->write("HTTP/1.1 " + QByteArray::number(reply.status) + " X\r\n"
                             "Content-Type: application/json\r\nContent-Length: "
                             + QByteArray::number(payload.size())
                             + "\r\nConnection: close\r\n\r\n" + payload);
                    s->disconnectFromHost();
                });
            }
        });
    }
    QString url() const { return u"http://127.0.0.1:%1"_s.arg(server.serverPort()); }
    int count(const QByteArray &method, const QByteArray &path) const
    {
        int n = 0;
        for (const Request &r : requests)
            n += r.method == method && r.path == path;
        return n;
    }
    QJsonObject bodyOf(const QByteArray &path) const
    {
        for (const Request &r : requests)
            if (r.method == "POST" && r.path == path)
                return r.body;
        return {};
    }

    QTcpServer server;
    std::function<Reply(const Request &)> route;
    QList<Request> requests;
};

/// Runs @p call on a worker and serves the mock on this thread meanwhile.
template <typename Result>
Result follow(std::function<Result()> call, std::function<void()> whileWaiting = {})
{
    QFutureWatcher<Result> watcher;
    watcher.setFuture(QtConcurrent::run(call));
    QElapsedTimer clock;
    clock.start();
    while (!watcher.isFinished() && clock.elapsed() < 20000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        if (whileWaiting)
            whileWaiting();
    }
    if (!watcher.isFinished()) {
        Result timedOut;
        timedOut.error = u"the test timed out"_s;
        return timedOut;
    }
    return watcher.result();
}

QJsonObject profileAnswer()
{
    const QJsonObject profile{
        { u"r_lo"_s, QJsonArray{ 1.0, 10.0 } },
        { u"r_hi"_s, QJsonArray{ 10.0, 100.0 } },
        { u"r_mid"_s, QJsonArray{ 3.1622776601683795, 31.622776601683793 } },
        { u"r_mean"_s, QJsonArray{ 4.0, QJsonValue() } },
        { u"complete"_s, QJsonArray{ true, false } },
        { u"count"_s, QJsonArray{ 120, 0 } },
        { u"mass"_s, QJsonArray{ 12.0, 0.0 } },
        { u"density"_s, QJsonArray{ 0.0029, 0.0 } },
        { u"enclosed_mass"_s, QJsonArray{ 13.0, 13.0 } },
        { u"inside_r_min"_s, QJsonObject{ { u"count"_s, 7 }, { u"mass"_s, 1.0 } } },
        { u"outside_r_max"_s, QJsonObject{ { u"count"_s, 30 } } },
        { u"selected_rows"_s, 157 },
        { u"r_min"_s, 1.0 },
        { u"r_max"_s, 100.0 },
        { u"complete_radius"_s, 50.0 },
        { u"columns"_s,
          QJsonObject{ { u"temperature"_s,
                         QJsonObject{ { u"mean"_s, QJsonArray{ 1e4, QJsonValue() } },
                                      { u"std"_s, QJsonArray{ 2e3, QJsonValue() } },
                                      { u"p16"_s, QJsonArray{ 8e3, QJsonValue() } },
                                      { u"median"_s, QJsonArray{ 9.5e3, QJsonValue() } },
                                      { u"p84"_s, QJsonArray{ 1.2e4, QJsonValue() } } } } } },
        { u"velocity"_s,
          QJsonObject{ { u"centre_velocity"_s, QJsonArray{ 1.0, 2.0, 3.0 } },
                       { u"centre_velocity_from"_s, u"mass-weighted mean inside 10"_s },
                       { u"axis"_s, QJsonArray{ 0.0, 0.6, 0.8 } },
                       { u"mean_radial"_s, QJsonArray{ -5.0, QJsonValue() } },
                       { u"mean_azimuthal"_s, QJsonArray{ 2.0, QJsonValue() } },
                       { u"sigma_r"_s, QJsonArray{ 100.0, QJsonValue() } },
                       { u"sigma_theta"_s, QJsonArray{ 90.0, QJsonValue() } },
                       { u"sigma_phi"_s, QJsonArray{ 80.0, QJsonValue() } },
                       { u"sigma_1d"_s, QJsonArray{ 90.4, QJsonValue() } },
                       { u"beta"_s, QJsonArray{ 0.27, QJsonValue() } } } },
    };
    return QJsonObject{
        { u"valid"_s, true },
        { u"error"_s, u""_s },
        { u"profile"_s, profile },
        { u"centre"_s, QJsonObject{ { u"mode"_s, u"shrinking_sphere"_s },
                                    { u"centre"_s, QJsonArray{ 10.0, 20.0, 30.0 } },
                                    { u"steps"_s, 42 } } },
        { u"units"_s, QJsonObject{ { u"radius"_s, u"kpc/h"_s },
                                   { u"mass"_s, u"1e10 Msun/h"_s },
                                   { u"density"_s, u"1e10 Msun/h / (kpc/h)^3"_s },
                                   { u"columns"_s, QJsonObject{ { u"temperature"_s, u"K"_s } } } } },
        { u"provenance"_s, QJsonObject{ { u"dataset"_s, u"snap_099.hdf5"_s },
                                        { u"selection"_s, QJsonArray{ u"x < 500"_s } } } },
    };
}

QJsonObject phaseAnswer()
{
    const QJsonObject diagram{
        { u"x_edges"_s, QJsonArray{ 1.0, 10.0, 100.0 } },
        { u"y_edges"_s, QJsonArray{ 0.0, 1.0, 2.0, 3.0 } },
        { u"counts"_s, QJsonArray{ QJsonArray{ 1, 2, 3 }, QJsonArray{ 4, 0, 6 } } },
        { u"weights"_s, QJsonArray{ QJsonArray{ 0.5, 1.0, 1.5 }, QJsonArray{ 2.0, 0.0, 3.0 } } },
        { u"selected_rows"_s, 25 },
        { u"selected_weight"_s, 11.0 },
        { u"binned_rows"_s, 16 },
        { u"binned_weight"_s, 8.0 },
        { u"outside_rows"_s, 5 },
        { u"outside_weight"_s, 2.0 },
        { u"dropped_rows"_s, 4 },
        { u"dropped_weight"_s, 1.0 },
        { u"invalid_weight_rows"_s, 0 },
    };
    return QJsonObject{ { u"valid"_s, true },
                        { u"error"_s, u""_s },
                        { u"diagram"_s, diagram },
                        { u"units"_s, QJsonObject{ { u"x"_s, u"1e10 Msun/h/(kpc/h)^3"_s },
                                                   { u"y"_s, u"K"_s },
                                                   { u"weight"_s, u"1e10 Msun/h"_s } } },
                        { u"provenance"_s, QJsonObject{ { u"dataset"_s, u"snap_099.hdf5"_s } } } };
}

QJsonObject taskStatus(const QString &status, double progress, const QJsonObject &result = {})
{
    QJsonObject o{ { u"valid"_s, true },     { u"error"_s, u""_s },
                   { u"task_id"_s, u"t1"_s }, { u"status"_s, status },
                   { u"progress"_s, progress } };
    if (!result.isEmpty())
        o.insert(u"result"_s, result);
    return o;
}

const QJsonObject kCreated{ { u"valid"_s, true }, { u"error"_s, u""_s },
                            { u"task_id"_s, u"t1"_s }, { u"status"_s, u"running"_s } };

std::vector<BackendTabularFilter> oneFilter()
{
    BackendTabularFilter f;
    f.field = u"x"_s;
    f.op = u"<"_s;
    f.value = u"500"_s;
    return { f };
}

}   // namespace

void TestParticleProfiles::theProfileBodyCarriesTheCentreAndOnlyTheRangesGiven()
{
    profiles::ProfileRequest req;
    req.datasetId = u"d1"_s;
    req.bins = 1000;   // clamped to the route's 256
    req.columns = { u"temperature"_s, u"radial_velocity"_s };
    QJsonObject body{ { u"dataset_id"_s, u"d1"_s }, { u"limit"_s, 100 }, { u"offset"_s, 0 } };
    profiles::addProfileFields(body, req);
    QVERIFY(!body.contains(u"limit"_s) && !body.contains(u"offset"_s));
    QCOMPARE(body.value(u"bins"_s).toInt(), 256);
    QCOMPARE(body.value(u"centre"_s).toObject().value(u"mode"_s).toString(),
             u"shrinking_sphere"_s);
    QVERIFY(!body.contains(u"r_min"_s) && !body.contains(u"r_max"_s));
    QCOMPARE(body.value(u"columns"_s).toArray().size(), 2);
    QVERIFY(!body.contains(u"axis"_s));   // the backend's angular momentum
    req.axis = std::array<double, 3>{ 0.0, 0.0, 1.0 };
    body = {};
    profiles::addProfileFields(body, req);
    QCOMPARE(body.value(u"axis"_s).toArray().at(2).toDouble(), 1.0);

    req.rMin = 0.5;
    req.rMax = 300.0;
    req.centre.mode = profiles::CentreMode::Point;
    req.centre.point = { 1.0, 2.0, 3.0 };
    body = {};
    profiles::addProfileFields(body, req);
    QCOMPARE(body.value(u"r_min"_s).toDouble(), 0.5);
    QCOMPARE(body.value(u"r_max"_s).toDouble(), 300.0);
    const QJsonObject point = body.value(u"centre"_s).toObject();
    QCOMPARE(point.value(u"mode"_s).toString(), u"point"_s);
    QCOMPARE(point.value(u"point"_s).toArray().at(2).toDouble(), 3.0);

    req.centre.mode = profiles::CentreMode::Target;
    req.centre.targetDatasetId = u"subfind1"_s;
    req.centre.targetRow = 12;
    QVERIFY(req.centre.usable());
    body = {};
    profiles::addProfileFields(body, req);
    const QJsonObject target = body.value(u"centre"_s).toObject();
    QCOMPARE(target.value(u"mode"_s).toString(), u"target"_s);
    QCOMPARE(target.value(u"target_dataset_id"_s).toString(), u"subfind1"_s);
    QCOMPARE(target.value(u"target_row"_s).toInteger(), 12);

    profiles::CentreChoice incomplete;
    incomplete.mode = profiles::CentreMode::Point;
    incomplete.point = { 1.0, profiles::kNaN, 3.0 };
    QVERIFY(!incomplete.usable());
}

void TestParticleProfiles::thePhaseBodySendsACentreOnlyForRadialAxes()
{
    profiles::PhaseRequest req;
    req.datasetId = u"d1"_s;
    req.x = { u"rho"_s, true, 64, 1e-3, 10.0 };
    req.y = { u"temperature"_s, true, 2000 };
    req.weightField = u"mass"_s;
    req.centre = profiles::CentreChoice{};
    QJsonObject body;
    profiles::addPhaseFields(body, req);
    const QJsonObject x = body.value(u"x"_s).toObject();
    QCOMPARE(x.value(u"scale"_s).toString(), u"log"_s);
    QCOMPARE(x.value(u"min"_s).toDouble(), 1e-3);
    QCOMPARE(x.value(u"max"_s).toDouble(), 10.0);
    const QJsonObject y = body.value(u"y"_s).toObject();
    QCOMPARE(y.value(u"bins"_s).toInt(), 512);
    QVERIFY(!y.contains(u"min"_s) && !y.contains(u"max"_s));   // both or neither
    QCOMPARE(body.value(u"weight_field"_s).toString(), u"mass"_s);
    QVERIFY(!body.contains(u"centre"_s));   // neither axis is radial

    req.x.field = u"radius"_s;
    body = {};
    profiles::addPhaseFields(body, req);
    QCOMPARE(body.value(u"centre"_s).toObject().value(u"mode"_s).toString(),
             u"shrinking_sphere"_s);
    QVERIFY(profiles::needsCentre(u"radial_velocity"_s));
    QVERIFY(!profiles::needsCentre(u"rho"_s));
}

void TestParticleProfiles::aProfileAnswerIsParsedWithEmptyShellsAsNaN()
{
    const profiles::ProfileResult r = profiles::parseProfile(profileAnswer());
    QVERIFY2(r.valid, qPrintable(r.error));
    QCOMPARE(r.rMid.size(), 2);
    QCOMPARE(r.complete, (QVector<bool>{ true, false }));
    QVERIFY(std::isnan(r.rMean[1]));
    QCOMPARE(r.selectedRows, 157);
    QCOMPARE(r.insideRMinCount, 7);
    QCOMPARE(r.outsideRMaxCount, 30);
    QCOMPARE(r.completeRadius, 50.0);
    QCOMPARE(r.columns.size(), 1);
    QCOMPARE(r.columns[0].name, u"temperature"_s);
    QCOMPARE(r.columns[0].median[0], 9.5e3);
    QVERIFY(std::isnan(r.columns[0].p84[1]));
    QVERIFY(r.velocity.present);
    QCOMPARE(r.velocity.centreVelocity[2], 3.0);
    QCOMPARE(r.velocity.beta[0], 0.27);
    QCOMPARE(r.velocity.axis[2], 0.8);
    QCOMPARE(profiles::vectorText(r.velocity.axis), u"(0, 0.6, 0.8)"_s);
    QCOMPARE(r.centre[1], 20.0);
    QCOMPARE(r.centreMode, u"shrinking_sphere"_s);
    QCOMPARE(profiles::unitOf(r.units, u"radius"_s), u"kpc/h"_s);
    QCOMPARE(profiles::unitOf(r.units, u"temperature"_s), u"K"_s);
    QCOMPARE(profiles::axisTitle(u"r"_s, u"kpc/h"_s), u"r [kpc/h]"_s);
    QCOMPARE(profiles::axisTitle(u"r"_s, QString()), u"r"_s);
}

void TestParticleProfiles::aProfileWithShellsOfDifferentLengthsIsRefused()
{
    QJsonObject answer = profileAnswer();
    QJsonObject profile = answer.value(u"profile"_s).toObject();
    profile.insert(u"density"_s, QJsonArray{ 1.0 });
    answer.insert(u"profile"_s, profile);
    const profiles::ProfileResult r = profiles::parseProfile(answer);
    QVERIFY(!r.valid);
    QVERIFY(!r.error.isEmpty());
    // The backend's own refusal comes through as it is.
    const profiles::ProfileResult refused = profiles::parseProfile(QJsonObject{
            { u"valid"_s, false }, { u"error"_s, u"Fewer than 100 particles are selected."_s } });
    QVERIFY(!refused.valid);
    QCOMPARE(refused.error, u"Fewer than 100 particles are selected."_s);
}

void TestParticleProfiles::aPhaseAnswerIsParsedAndItsTotalsClose()
{
    const profiles::PhaseResult r = profiles::parsePhase(phaseAnswer());
    QVERIFY2(r.valid, qPrintable(r.error));
    QCOMPARE(r.counts.size(), 2);
    QCOMPARE(r.counts[1][2], 6.0);
    QCOMPARE(r.weights[0][1], 1.0);
    QCOMPARE(profiles::closureGap(r), 0);
    const QString text = profiles::closureText(r, true);
    QVERIFY(text.contains(u"25 selected rows"_s));
    QVERIFY(text.contains(u"16 binned"_s));
    QVERIFY(text.contains(u"5 outside"_s));
    QVERIFY(text.contains(u"4 dropped"_s));
    QVERIFY(text.contains(u"[1e10 Msun/h]"_s));
    QVERIFY(!profiles::closureText(r, false).contains(u"Weight"_s));

    QJsonObject open = phaseAnswer();
    QJsonObject diagram = open.value(u"diagram"_s).toObject();
    diagram.insert(u"dropped_rows"_s, 1);
    diagram.insert(u"invalid_weight_rows"_s, 3);
    open.insert(u"diagram"_s, diagram);
    const profiles::PhaseResult invalid = profiles::parsePhase(open);
    QCOMPARE(profiles::closureGap(invalid), 0);   // 16 + 5 + 1 + 3 invalid weights = 25
    QVERIFY(profiles::closureText(invalid, true).contains(u"3 rows have an invalid weight"_s));
    diagram.insert(u"invalid_weight_rows"_s, 0);
    open.insert(u"diagram"_s, diagram);
    QCOMPARE(profiles::closureGap(profiles::parsePhase(open)), 3);

    QJsonObject broken = phaseAnswer();
    diagram = broken.value(u"diagram"_s).toObject();
    diagram.insert(u"counts"_s, QJsonArray{ QJsonArray{ 1, 2 } });
    broken.insert(u"diagram"_s, diagram);
    QVERIFY(!profiles::parsePhase(broken).valid);
}

void TestParticleProfiles::theCsvFilesCarryTheirProvenance()
{
    const QStringList profile = profiles::profileCsv(profiles::parseProfile(profileAnswer()))
                                        .split(u'\n', Qt::SkipEmptyParts);
    QVERIFY(profile.first().startsWith(u"# "_s));
    QVERIFY(profile.join(u'\n').contains(u"# dataset,snap_099.hdf5"_s));
    QVERIFY(profile.join(u'\n').contains(u"# units,"_s));
    const auto header = std::find_if(profile.begin(), profile.end(),
                                     [](const QString &l) { return l.startsWith(u"r_lo,"_s); });
    QVERIFY(header != profile.end());
    QVERIFY(header->contains(u"temperature_median"_s));
    QVERIFY(header->contains(u"beta"_s));
    QVERIFY(profile.join(u'\n').contains(u"# velocity_axis,(0; 0.6; 0.8)"_s));
    QVERIFY(profile.join(u'\n').contains(u"peculiar"_s));
    QCOMPARE(profile.end() - header - 1, 2);   // one row per shell
    QCOMPARE(header->count(u','), (header + 1)->count(u','));

    const QStringList phase =
            profiles::phaseCsv(profiles::parsePhase(phaseAnswer()), true).split(u'\n', Qt::SkipEmptyParts);
    QVERIFY(phase.join(u'\n').contains(u"# closure,"_s));
    const auto phaseHeader = std::find_if(phase.begin(), phase.end(), [](const QString &l) {
        return l.startsWith(u"x_lo,"_s);
    });
    QVERIFY(phaseHeader != phase.end());
    QCOMPARE(phase.end() - phaseHeader - 1, 6);   // 2 × 3 cells
}

void TestParticleProfiles::theProfileRunsAsATask()
{
    int polls = 0;
    MockBackend backend([&polls](const Request &r) -> Reply {
        if (r.method == "POST" && r.path == "/v1/tasks/catalogue_profile")
            return { 200, kCreated };
        if (r.method == "GET" && r.path == "/v1/tasks/t1")
            return { 200, ++polls < 2 ? taskStatus(u"running"_s, 0.4)
                                      : taskStatus(u"completed"_s, 1.0, profileAnswer()) };
        return { 404, QJsonObject{ { u"detail"_s, u"Not Found"_s } } };
    });
    QVERIFY(backend.server.isListening());
    profiles::ProfileRequest req;
    req.datasetId = u"d1"_s;
    req.columns = { u"temperature"_s };
    visivo::BackendTaskControl control;
    const QString url = backend.url();
    const profiles::ProfileResult r = follow<profiles::ProfileResult>([url, req, &control]() {
        return BackendClient(url).catalogueProfileFollowingTask(req, oneFilter(), &control);
    });
    QVERIFY2(r.valid, qPrintable(r.error));
    QCOMPARE(r.selectedRows, 157);
    const QJsonObject body = backend.bodyOf("/v1/tasks/catalogue_profile");
    QCOMPARE(body.value(u"dataset_id"_s).toString(), u"d1"_s);
    QCOMPARE(body.value(u"filters"_s).toArray().size(), 1);
    QCOMPARE(body.value(u"filters"_s).toArray().at(0).toObject().value(u"field"_s).toString(),
             u"x"_s);
    QCOMPARE(body.value(u"centre"_s).toObject().value(u"mode"_s).toString(),
             u"shrinking_sphere"_s);
    QVERIFY(!body.contains(u"limit"_s));
    QCOMPARE(backend.count("DELETE", "/v1/tasks/t1"), 0);
}

void TestParticleProfiles::thePhaseDiagramRunsAsATaskAndCancels()
{
    {
        MockBackend backend([](const Request &r) -> Reply {
            if (r.method == "POST" && r.path == "/v1/tasks/catalogue_phase_diagram")
                return { 200, kCreated };
            return { 200, taskStatus(u"completed"_s, 1.0, phaseAnswer()) };
        });
        profiles::PhaseRequest req;
        req.datasetId = u"d1"_s;
        req.x = { u"rho"_s, true, 2 };
        req.y = { u"temperature"_s, false, 3 };
        visivo::BackendTaskControl control;
        const QString url = backend.url();
        const profiles::PhaseResult r = follow<profiles::PhaseResult>([url, req, &control]() {
            return BackendClient(url).cataloguePhaseDiagramFollowingTask(req, {}, &control);
        });
        QVERIFY2(r.valid, qPrintable(r.error));
        QCOMPARE(r.binnedRows, 16);
        QCOMPARE(backend.bodyOf("/v1/tasks/catalogue_phase_diagram")
                         .value(u"x"_s).toObject().value(u"field"_s).toString(),
                 u"rho"_s);
    }
    MockBackend backend([](const Request &r) -> Reply {
        if (r.method == "POST")
            return { 200, kCreated };
        if (r.method == "DELETE")
            return { 200, QJsonObject{ { u"valid"_s, true }, { u"cancelled"_s, true } } };
        return { 200, taskStatus(u"running"_s, 0.2) };
    });
    profiles::PhaseRequest req;
    req.datasetId = u"d1"_s;
    req.x = { u"rho"_s };
    req.y = { u"temperature"_s };
    visivo::BackendTaskControl control;
    const QString url = backend.url();
    const profiles::PhaseResult r = follow<profiles::PhaseResult>(
            [url, req, &control]() {
                return BackendClient(url).cataloguePhaseDiagramFollowingTask(req, {}, &control);
            },
            [&]() {
                if (backend.count("GET", "/v1/tasks/t1") >= 1)
                    control.requestAbort();
            });
    QVERIFY(r.cancelled);
    QVERIFY(!r.valid);
    QCOMPARE(backend.count("DELETE", "/v1/tasks/t1"), 1);
}

void TestParticleProfiles::anOlderBackendIsToldToUpdate()
{
    MockBackend backend([](const Request &) -> Reply {
        return { 404, QJsonObject{ { u"detail"_s, u"Not Found"_s } } };
    });
    profiles::ProfileRequest req;
    req.datasetId = u"d1"_s;
    visivo::BackendTaskControl control;
    const QString url = backend.url();
    const profiles::ProfileResult r = follow<profiles::ProfileResult>([url, req, &control]() {
        return BackendClient(url).catalogueProfileFollowingTask(req, {}, &control);
    });
    QVERIFY(!r.valid);
    QVERIFY(!r.cancelled);
    QVERIFY2(r.error.contains(u"update"_s), qPrintable(r.error));
    QCOMPARE(backend.count("POST", "/v1/catalogue/profile"), 0);
}
