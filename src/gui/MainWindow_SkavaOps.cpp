// Operations next to the data for a dataset found through SKAVA: a SODA
// cutout, a moment map, a PV diagram or a source finder, run by the VisIVO
// backend on the node that holds the replica, through the services SKAVA's
// DataLink advertises for it. The dataset never leaves the node; the product
// is opened in place.
#include "MainWindow.h"

#include "../Settings.h"
#include "../skava/BackendRouting.h"
#include "../skava/BrokerClient.h"
#include "../skava/NodeSoda.h"
#include "../skava/SkavaClient.h"
#include "SkavaSourceFindDialog.h"
#include "app/BackendClient.h"
#include "vtkWindowCatalogue3D.h"

#include <QComboBox>
#include <QDebug>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleValidator>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QIntValidator>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QRegularExpression>
#include <QSpinBox>
#include <QThread>
#include <QtConcurrentRun>

#include <atomic>
#include <cmath>
#include <functional>
#include <memory>

using namespace Qt::StringLiterals;

namespace {

/// Run @p work on a worker thread behind a cancellable progress dialog and
/// hand the result to @p done on the UI thread. Cancel sets the flag the
/// worker watches: a job on a node is aborted there, not just forgotten.
template <typename T>
void runJobWithProgress(QWidget *parent, const QString &title, const QString &label,
                        std::function<T(std::atomic_bool *)> work,
                        std::function<void(const T &)> done)
{
    auto cancel = std::make_shared<std::atomic_bool>(false);
    auto *progress = new QProgressDialog(label, QObject::tr("Cancel"), 0, 0, parent);
    progress->setWindowTitle(title);
    progress->setWindowModality(Qt::WindowModal);
    progress->setMinimumDuration(400);
    progress->setAttribute(Qt::WA_DeleteOnClose);
    auto *watcher = new QFutureWatcher<T>(parent);
    // The window going away (the application quitting) is a cancel too:
    // the worker must not keep a job alive that nobody will read.
    QObject::connect(parent, &QObject::destroyed, [cancel]() { cancel->store(true); });
    QObject::connect(progress, &QProgressDialog::canceled, watcher, [watcher, progress, cancel]() {
        cancel->store(true);      // the worker aborts the job and returns; nobody reads the result
        QObject::disconnect(watcher, nullptr, nullptr, nullptr);
        watcher->deleteLater();
        // Cancel only hides the dialog (QProgressDialog::cancel → reset/hide), so
        // WA_DeleteOnClose never fires on this path: free it explicitly.
        progress->deleteLater();
    });
    QObject::connect(watcher, &QFutureWatcher<T>::finished, parent, [watcher, progress, done]() {
        const T result = watcher->result();
        watcher->deleteLater();
        progress->close();
        done(result);
    });
    watcher->setFuture(QtConcurrent::run([work, cancel]() { return work(cancel.get()); }));
}

/// Where a node service is called from this desktop: the route to the
/// node's registered backend (its token) and the service's URL on it.
struct NodeServiceTarget
{
    SkavaRoute route;
    QString serviceUrl;
    QStringList why;       ///< when there is no route: the descriptors passed over
    bool valid() const { return route.valid() && !serviceUrl.isEmpty(); }
};

NodeServiceTarget targetFor(Settings *settings, const SkavaDatalinkResponse &datalink,
                            const SkavaNodeService *svc)
{
    NodeServiceTarget t;
    if (!svc)
        return t;
    const SkavaRouting routing = routeSkavaCandidates(settings, datalink, svc->nodeCode);
    t.why = routing.skipped;
    for (const auto &candidate : routing.candidates) {
        if (candidate.nodeCode == svc->nodeCode) {
            t.route = candidate;
            break;
        }
    }
    // The service's path under its own backend (the descriptor's endpoint),
    // on the backend registered for the node.
    if (t.route.valid())
        t.serviceUrl = NodeSodaClient::serviceUrlOn(t.route.node.url, svc->accessUrl, t.route.endpoint);
    return t;
}

QString nodeLabelOf(const SkavaRoute &route)
{
    return route.node.name.isEmpty() ? route.node.id : route.node.name;
}

void explainNoRoute(QWidget *parent, const QString &title, const QString &obsId, const QStringList &why)
{
    QMessageBox::information(
            parent, title,
            u"“%1” can only be processed by the VisIVO backend of the node that holds it, "
            u"and no registered backend can reach that node.%2\n\nRegister the node's backend "
            u"in Settings ▸ Backends (Open offers to), then try again."_s
                    .arg(obsId, why.isEmpty() ? QString() : u"\n\n• "_s + why.join(u"\n• "_s)));
}

QLineEdit *numberEdit(QWidget *parent, double value, const QString &placeholder)
{
    auto *e = new QLineEdit(parent);
    e->setPlaceholderText(placeholder);
    e->setValidator(new QDoubleValidator(parent));
    if (std::isfinite(value))
        e->setText(QString::number(value, 'g', 10));
    return e;
}

bool readNumber(QLineEdit *e, double &out)
{
    const QString t = e->text().trimmed();
    if (t.isEmpty())
        return false;
    bool ok = false;
    out = t.toDouble(&ok);
    return ok;
}

/// "min max" from two edits, lowest first; empty unless both are numbers.
QString rangeOf(QLineEdit *lo, QLineEdit *hi)
{
    double a = 0, b = 0;
    if (!readNumber(lo, a) || !readNumber(hi, b))
        return {};
    return u"%1 %2"_s.arg(QString::number(std::min(a, b), 'g', 10), QString::number(std::max(a, b), 'g', 10));
}

/// Numbers typed as a list ("ra dec ra dec …", commas allowed), or empty
/// when any token is not a number.
QStringList numberList(const QString &text)
{
    QStringList out;
    const QStringList tokens = text.split(QRegularExpression(u"[\\s,]+"_s), Qt::SkipEmptyParts);
    for (const QString &tok : tokens) {
        bool ok = false;
        const double v = tok.toDouble(&ok);
        if (!ok)
            return {};
        out << QString::number(v, 'g', 10);
    }
    return out;
}

/// The job's parameters start with the dataset's identifier on the node:
/// the one the service fixes, else the replica's own URL there.
NodeSodaClient::Params baseParams(const SkavaNodeService &svc, const SkavaRoute &route)
{
    NodeSodaClient::Params params;
    params.append({ u"ID"_s, svc.inputParams.value(u"ID"_s, route.accessUrl) });
    for (auto it = svc.inputParams.cbegin(); it != svc.inputParams.cend(); ++it) {
        if (it.key() != "ID"_L1)
            params.append({ it.key(), it.value() });
    }
    return params;
}

}  // namespace

void MainWindow::doSkavaNodeOperation(const QString &kind, const QString &obsId,
                                      const SkavaDatalinkResponse &datalink)
{
    if (kind == "cutout"_L1)
        doSkavaSodaSubset(obsId, datalink);
    else if (kind == "moment"_L1 || kind == "pv"_L1)
        doSkavaNodeProduct(kind, obsId, datalink);
    else if (kind == "sourcefind"_L1)
        doSkavaSourceFind(obsId, datalink);
    else
        qWarning().noquote() << u"[skava] unknown node operation"_s << kind;
}

void MainWindow::doSkavaSodaSubset(const QString &obsId, const SkavaDatalinkResponse &datalink)
{
    // The cutout runs where the data lives: the node's SODA job service when
    // SKAVA advertises one (the product stays in the node's workspace, where
    // this desktop opens it), else the routed backend's own cutout route.
    const SkavaNodeService *svc = datalink.service(u"cutout-async"_s);
    NodeServiceTarget target = targetFor(this->settings, datalink, svc);
    SkavaRoute route = target.route;
    if (!svc) {
        route = routeSkavaDataset(this->settings, datalink);
        target.why = route.skipped;
    }
    if (!route.valid() || !route.accessUrl.startsWith(QStringLiteral("file://"))) {
        explainNoRoute(this, u"SODA subset unavailable"_s, obsId, target.why);
        return;
    }
    const QString nodeLabel = nodeLabelOf(route);

    // ── Region dialog (POS + BAND + TIME), prefilled from the discovery row ──
    const SkavaDatasetMetadata &m = datalink.dataset;
    QDialog dlg(this);
    dlg.setWindowTitle(u"SODA subset - %1"_s.arg(obsId));
    auto *form = new QFormLayout(&dlg);
    auto *shape = new QComboBox(&dlg);
    shape->addItems({ u"CIRCLE"_s, u"POLYGON"_s, u"(no spatial subset)"_s });
    form->addRow(u"POS shape"_s, shape);
    auto *raEdit   = numberEdit(&dlg, m.sRa,  u"RA (deg)"_s);
    auto *decEdit  = numberEdit(&dlg, m.sDec, u"Dec (deg)"_s);
    auto *radEdit  = numberEdit(&dlg, std::isfinite(m.sFov) ? m.sFov * 0.5 : 0.1, u"radius (deg)"_s);
    auto *polyEdit = new QLineEdit(&dlg);
    polyEdit->setPlaceholderText(u"ra1 dec1 ra2 dec2 ra3 dec3 … (deg, at least three vertices)"_s);
    auto *bMinEdit = numberEdit(&dlg, m.emMin, u"min (m)"_s);
    auto *bMaxEdit = numberEdit(&dlg, m.emMax, u"max (m)"_s);
    auto *tMinEdit = numberEdit(&dlg, std::numeric_limits<double>::quiet_NaN(), u"MJD min"_s);
    auto *tMaxEdit = numberEdit(&dlg, std::numeric_limits<double>::quiet_NaN(), u"MJD max"_s);
    form->addRow(u"CIRCLE, RA (deg)"_s, raEdit);
    form->addRow(u"Dec (deg)"_s, decEdit);
    form->addRow(u"Radius (deg)"_s, radEdit);
    form->addRow(u"POLYGON vertices"_s, polyEdit);
    form->addRow(u"BAND, min (m)"_s, bMinEdit);
    form->addRow(u"BAND, max (m)"_s, bMaxEdit);
    form->addRow(u"TIME, MJD min"_s, tMinEdit);
    form->addRow(u"TIME, MJD max"_s, tMaxEdit);
    auto syncShape = [=](int index) {
        const bool circle = index == 0, polygon = index == 1;
        raEdit->setEnabled(circle);
        decEdit->setEnabled(circle);
        radEdit->setEnabled(circle);
        polyEdit->setEnabled(polygon);
    };
    QObject::connect(shape, &QComboBox::currentIndexChanged, &dlg, syncShape);
    syncShape(0);
    auto *hint = new QLabel(
            u"Leave an axis blank to omit it; at least one of POS / BAND / TIME is required. "
            u"TIME applies to a cube with a time axis. The cutout runs on node %1, next to the "
            u"data%2."_s.arg(nodeLabel, svc ? u" (SODA job)"_s : QString()), &dlg);
    hint->setWordWrap(true);
    form->addRow(hint);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    buttons->button(QDialogButtonBox::Ok)->setText(u"Run cutout"_s);
    form->addRow(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted) return;

    QString pos;
    if (shape->currentIndex() == 0) {
        double ra = 0, dec = 0, rad = 0;
        if (readNumber(raEdit, ra) && readNumber(decEdit, dec) && readNumber(radEdit, rad))
            pos = u"CIRCLE %1 %2 %3"_s.arg(QString::number(ra, 'g', 10), QString::number(dec, 'g', 10),
                                            QString::number(rad, 'g', 10));
    } else if (shape->currentIndex() == 1) {
        const QStringList numbers = numberList(polyEdit->text());
        if (numbers.size() < 6 || numbers.size() % 2) {
            QMessageBox::information(this, u"SODA subset"_s,
                                     u"A POLYGON needs at least three vertices, as 'ra dec' pairs in degrees."_s);
            return;
        }
        pos = u"POLYGON "_s + numbers.join(u' ');
    }
    const QString band = rangeOf(bMinEdit, bMaxEdit);
    const QString time = rangeOf(tMinEdit, tMaxEdit);
    if (pos.isEmpty() && band.isEmpty() && time.isEmpty()) {
        QMessageBox::information(this, u"SODA subset"_s,
                                 u"Provide a POS, a BAND and/or a TIME range to subset."_s);
        return;
    }

    // ── Cutout + open on the node, off the UI thread ────────────────────────
    this->setActiveBackendBadge(route.node, route.rankReason);
    const QString url = route.node.url;
    const QString token = route.node.token;
    const QString accessUrl = route.accessUrl;
    struct Outcome { bool valid{ false }; QString error; QString path; QString applied; BackendOpenDatasetResult opened; };
    std::function<Outcome(std::atomic_bool *)> work;
    if (svc) {
        NodeSodaClient::Params params = baseParams(*svc, route);
        if (!pos.isEmpty())  params.append({ u"POS"_s, pos });
        if (!band.isEmpty()) params.append({ u"BAND"_s, band });
        if (!time.isEmpty()) params.append({ u"TIME"_s, time });
        const QString serviceUrl = target.serviceUrl;
        work = [url, token, serviceUrl, params](std::atomic_bool *cancel) {
            Outcome o;
            const NodeSodaJobResult job = NodeSodaClient(serviceUrl, token).run(params, cancel);
            if (!job.valid) {
                o.error = job.error;
                return o;
            }
            if (job.outputPath.isEmpty()) {
                o.error = job.noData.isEmpty() ? u"The selection is empty."_s : job.noData;
                return o;
            }
            o.valid = true;
            o.path = job.outputPath;
            o.applied = job.applied;
            // The cutout stays on the node: open it there, never downloaded.
            o.opened = BackendClient(url, token).openDataset(o.path);
            return o;
        };
    } else {
        work = [url, token, obsId, accessUrl, pos, band, time](std::atomic_bool *) {
            Outcome o;
            BackendClient bc(url, token);
            const BackendSodaCutoutResult cut = bc.sodaCutout(obsId, accessUrl, pos, band, time);
            if (!cut.valid) {
                o.error = cut.error;
                return o;
            }
            o.valid = true;
            o.path = cut.path;
            o.applied = cut.applied;
            o.opened = bc.openDataset(o.path);
            return o;
        };
    }
    runJobWithProgress<Outcome>(
            this, u"SODA subset"_s, u"Cutting %1 on %2…"_s.arg(obsId, nodeLabel), work,
            [this, url, token, obsId](const Outcome &o) {
                if (!o.valid) {
                    QMessageBox::critical(this, u"SODA subset failed"_s,
                                          o.error.isEmpty() ? u"The node returned no detail."_s : o.error);
                    return;
                }
                if (!o.opened.valid) {
                    QMessageBox::critical(this, u"Could not open SODA cutout"_s,
                                          o.opened.error.isEmpty() ? u"Backend returned no error detail."_s
                                                                   : o.opened.error);
                    return;
                }
                BackendClient client(url, token);
                // The viewer and the session context read the client's own session id,
                // which a fresh client lacks: hand it the one the open was made in.
                client.setSessionId(o.opened.sessionId);
                openViewerFromOpenedDataset(o.path, o.opened, client);
                if (this->recentMgr)
                    this->recentMgr->record(o.path, u"%1 [SODA cutout]"_s.arg(obsId), o.opened.kind);
            });
}

void MainWindow::doSkavaNodeProduct(const QString &kind, const QString &obsId,
                                    const SkavaDatalinkResponse &datalink)
{
    const bool moment = kind == "moment"_L1;
    const QString title = moment ? u"Moment map next to the data"_s : u"PV diagram next to the data"_s;
    const SkavaNodeService *svc = datalink.service(kind);
    if (!svc) {
        QMessageBox::information(
                this, title,
                u"SKAVA advertises no %1 service for “%2”: its node does not compute it in place "
                u"(the dataset is not a cube, or the node's backend predates the operation). "
                u"Open the dataset and compute it in the viewer instead."_s
                        .arg(moment ? u"moment"_s : u"PV"_s, obsId));
        return;
    }
    const NodeServiceTarget target = targetFor(this->settings, datalink, svc);
    if (!target.valid()) {
        explainNoRoute(this, title, obsId, target.why);
        return;
    }
    const SkavaRoute route = target.route;
    const QString nodeLabel = nodeLabelOf(route);
    const SkavaDatasetMetadata &m = datalink.dataset;

    // ── Parameters ──────────────────────────────────────────────────────────
    QDialog dlg(this);
    dlg.setWindowTitle(u"%1 - %2"_s.arg(title, obsId));
    auto *form = new QFormLayout(&dlg);
    QComboBox *orderCombo = nullptr;
    QLineEdit *chanFirst = nullptr, *chanLast = nullptr, *threshold = nullptr, *pathEdit = nullptr;
    QSpinBox *widthSpin = nullptr;
    QString product;
    if (moment) {
        orderCombo = new QComboBox(&dlg);
        orderCombo->addItems({ u"0 (integrated intensity)"_s, u"1 (velocity field)"_s,
                               u"2 (velocity dispersion)"_s });
        form->addRow(u"Moment order"_s, orderCombo);
        chanFirst = new QLineEdit(&dlg);
        chanFirst->setPlaceholderText(u"first channel (0-based), blank = all"_s);
        chanFirst->setValidator(new QIntValidator(0, 1'000'000, &dlg));
        chanLast = new QLineEdit(&dlg);
        chanLast->setPlaceholderText(u"last channel (inclusive)"_s);
        chanLast->setValidator(new QIntValidator(0, 1'000'000, &dlg));
        form->addRow(u"Channels, first"_s, chanFirst);
        form->addRow(u"last"_s, chanLast);
        threshold = numberEdit(&dlg, std::numeric_limits<double>::quiet_NaN(),
                               u"mask voxels below this value (blank = none)"_s);
        form->addRow(u"Threshold"_s, threshold);
    } else {
        pathEdit = new QLineEdit(&dlg);
        pathEdit->setPlaceholderText(u"ra1 dec1 ra2 dec2 … (deg)"_s);
        // A first guess: a horizontal path across the middle of the field.
        if (std::isfinite(m.sRa) && std::isfinite(m.sDec)) {
            const double half = std::isfinite(m.sFov) ? m.sFov * 0.25 : 0.05;
            const double cosDec = std::max(0.1, std::cos(m.sDec * M_PI / 180.0));
            pathEdit->setText(u"%1 %2 %3 %4"_s.arg(QString::number(m.sRa - half / cosDec, 'g', 10),
                                                    QString::number(m.sDec, 'g', 10),
                                                    QString::number(m.sRa + half / cosDec, 'g', 10),
                                                    QString::number(m.sDec, 'g', 10)));
        }
        form->addRow(u"Path vertices"_s, pathEdit);
        widthSpin = new QSpinBox(&dlg);
        widthSpin->setRange(1, 200);
        widthSpin->setValue(3);
        widthSpin->setSuffix(u" px"_s);
        form->addRow(u"Slice width"_s, widthSpin);
    }
    auto *hint = new QLabel(u"Computed by the VisIVO backend on node %1, next to the data (SODA job, "
                            u"OPERATION=%2); the product opens from there."_s
                                    .arg(nodeLabel, svc->inputParams.value(u"OPERATION"_s)), &dlg);
    hint->setWordWrap(true);
    form->addRow(hint);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    buttons->button(QDialogButtonBox::Ok)->setText(u"Compute"_s);
    form->addRow(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted) return;

    NodeSodaClient::Params params = baseParams(*svc, route);
    if (moment) {
        const int order = orderCombo->currentIndex();
        params.append({ u"MOMENT"_s, QString::number(order) });
        const QString first = chanFirst->text().trimmed(), last = chanLast->text().trimmed();
        if (!first.isEmpty() || !last.isEmpty()) {
            if (first.isEmpty() || last.isEmpty()) {
                QMessageBox::information(this, title, u"Give both the first and the last channel, or neither."_s);
                return;
            }
            params.append({ u"CHANNELS"_s, u"%1 %2"_s.arg(first, last) });
        }
        double thr = 0;
        if (readNumber(threshold, thr))
            params.append({ u"THRESHOLD"_s, QString::number(thr, 'g', 10) });
        product = u"moment %1"_s.arg(order);
    } else {
        const QStringList numbers = numberList(pathEdit->text());
        if (numbers.size() < 4 || numbers.size() % 2) {
            QMessageBox::information(this, title, u"The path needs at least two vertices, as 'ra dec' pairs in degrees."_s);
            return;
        }
        params.append({ u"PVPATH"_s, numbers.join(u' ') });
        params.append({ u"PVWIDTH"_s, QString::number(widthSpin->value()) });
        product = u"PV diagram"_s;
    }

    // ── The job, then the product opened on the node ────────────────────────
    this->setActiveBackendBadge(route.node, route.rankReason);
    const QString url = route.node.url;
    const QString token = route.node.token;
    const QString serviceUrl = target.serviceUrl;
    runJobWithProgress<NodeSodaJobResult>(
            this, title, u"Computing the %1 of %2 on %3…"_s.arg(product, obsId, nodeLabel),
            [serviceUrl, token, params](std::atomic_bool *cancel) {
                return NodeSodaClient(serviceUrl, token).run(params, cancel);
            },
            [this, url, token, obsId, product, nodeLabel, title](const NodeSodaJobResult &job) {
                if (!job.valid) {
                    QMessageBox::critical(this, title, job.error.isEmpty() ? u"The node returned no detail."_s
                                                                           : job.error);
                    return;
                }
                if (job.outputPath.isEmpty()) {
                    QMessageBox::information(this, title, job.noData.isEmpty() ? u"The job produced nothing."_s
                                                                               : job.noData);
                    return;
                }
                openBackendProductFits(url, token, job.outputPath,
                                       u"%1 [%2 on %3]"_s.arg(obsId, product, nodeLabel));
            });
}

namespace {

/// The registered backend of a node the broker placed a job on: by SKAVA
/// node code, else by the backend URL the broker reports. ``registry`` is a
/// copy taken on the UI thread: the worker never touches Settings.
BackendNode registeredBackendFor(const QList<BackendNode> &registry, const QString &nodeCode,
                                 const QString &backendUrl)
{
    SkavaVisivoBackendDescriptor desc;
    desc.endpoint = backendUrl;
    desc.nodeCode = nodeCode;
    const int idx = findRegisteredBackend(registry, desc);
    return idx >= 0 ? registry[idx] : BackendNode{};
}

constexpr int kTaskPollMs = 500;
constexpr int kTaskCancelCheckMs = 100;
/// Consecutive status requests that may fail before the task is given up
/// (a backend restarted, the task evicted: asking again changes nothing).
constexpr int kMaxTaskPollFailures = 10;

/// A finder task on the node's backend, followed to its end. ``log``
/// receives the progress; ``cancel`` cancels the task on the node.
SkavaFinderOutcome runFinderTask(const BackendClient &client, const QString &datasetId,
                                 const QString &finder, const QJsonObject &params,
                                 std::atomic_bool *cancel,
                                 const std::function<void(const QString &)> &log)
{
    SkavaFinderOutcome o;
    o.finder = finder;
    const BackendTaskCreateResult created = client.createSourceFindTask(datasetId, finder, params);
    if (!created.valid) {
        o.error = created.error.isEmpty() ? u"The node did not start the finder."_s : created.error;
        return o;
    }
    log(u"task %1 started on the node"_s.arg(created.taskId));
    double lastProgress = -1;
    int failures = 0;
    auto cancelled = [&]() {
        QString cancelError;
        const bool confirmed = client.cancelTask(created.taskId, cancelError);
        o.error = confirmed ? u"cancelled"_s
                            : u"cancelled, but the node did not confirm it%1; the task may still be running"_s.arg(
                                      cancelError.isEmpty() ? QString() : u" ("_s + cancelError + u")"_s);
        return o;
    };
    for (;;) {
        for (int waited = 0; waited < kTaskPollMs; waited += kTaskCancelCheckMs) {
            if (cancel && cancel->load())
                return cancelled();
            QThread::msleep(kTaskCancelCheckMs);
        }
        const BackendTaskStatusResult status = client.requestTaskStatus(created.taskId);
        if (!status.valid && status.status.isEmpty()) {
            // A poll that failed is not a task that failed, within bounds;
            // past them the task is lost to us and stopped if it can be.
            if (++failures >= kMaxTaskPollFailures) {
                QString cancelError;
                const bool stopped = client.cancelTask(created.taskId, cancelError);
                o.error = u"lost track of task %1 on the node (%2); %3"_s.arg(
                        created.taskId, status.error,
                        stopped ? u"the node accepted its cancellation"_s
                                : u"it could not be cancelled and may still be running%1"_s.arg(
                                          cancelError.isEmpty() ? QString() : u" ("_s + cancelError + u")"_s));
                return o;
            }
            log(u"(poll) "_s + status.error);
            continue;
        }
        failures = 0;
        if (status.progress != lastProgress && status.status == "running"_L1) {
            lastProgress = status.progress;
            log(u"running… %1%"_s.arg(static_cast<int>(status.progress * 100)));
        }
        if (status.status == "completed"_L1) {
            const QJsonObject r = status.resultObject;
            o.valid = true;
            o.nSources = r.value(u"n_sources"_s).toInt(-1);
            o.maskPath = r.value(u"mask_path"_s).toString();
            o.catalogPath = r.value(u"catalog_path"_s).toString();
            return o;
        }
        if (status.status == "failed"_L1 || status.status == "cancelled"_L1) {
            o.error = status.error.isEmpty() ? u"the task %1"_s.arg(status.status) : status.error;
            return o;
        }
    }
}

}  // namespace

void MainWindow::doSkavaSourceFind(const QString &obsId, const SkavaDatalinkResponse &datalink)
{
    const QString title = u"Source finder next to the data"_s;
    const QString brokerUrl = this->settings->getSkavaBrokerUrl();
    // Where the finder runs: the Computing Broker places the job itself
    // (asking SKAVA), else the node SKAVA ranked first, as a task on its
    // backend. Either way the products are opened from the node's backend,
    // so that backend has to be registered.
    const SkavaRoute route = routeSkavaDataset(this->settings, datalink);
    const bool direct = brokerUrl.isEmpty();
    if (direct && (!route.valid() || !route.accessUrl.startsWith(QStringLiteral("file://")))) {
        explainNoRoute(this, title, obsId, route.skipped);
        return;
    }
    const QString kind = datalink.dataset.dataproductType.trimmed().toLower();
    const QString skavaToken = this->settings->getSkavaToken();
    const QString nodeLabel = route.valid() ? nodeLabelOf(route) : QString();
    const QString where = direct
            ? u"The finder runs as a task of the VisIVO backend on node %1, which SKAVA ranked first "
              u"for this dataset%2. The dataset is opened there; nothing is transferred."_s
                      .arg(nodeLabel, route.rankReason.isEmpty() ? QString() : u" ("_s + route.rankReason + u")"_s)
            : u"The job goes to the Computing Broker at %1, which asks SKAVA for the node to run it on "
              u"and starts the finder on that node's VisIVO backend. Nothing is transferred."_s.arg(brokerUrl);

    // The finders the node offers, read from its backend when the job runs
    // there directly; the broker chooses the node itself.
    const QString url = route.valid() ? route.node.url : QString();
    const QString token = route.valid() ? route.node.token : QString();
    const QString accessUrl = route.accessUrl;
    runJobWithProgress<BackendFindersResult>(
            this, title, u"Asking %1 which finders it offers…"_s.arg(nodeLabel.isEmpty() ? u"the node"_s : nodeLabel),
            [url, token, direct](std::atomic_bool *) {
                return direct && !url.isEmpty() ? BackendClient(url, token).listFinders() : BackendFindersResult{};
            },
            [this, title, obsId, route, direct, brokerUrl, skavaToken, kind, where, url, token,
             accessUrl, nodeLabel](const BackendFindersResult &finders) {
                if (direct && !finders.valid) {
                    QMessageBox::critical(this, title,
                                          u"%1 does not run source finders: %2"_s.arg(
                                                  nodeLabel, finders.error.isEmpty() ? u"no answer"_s : finders.error));
                    return;
                }
                SkavaSourceFindDialog::Runner runner;
                if (direct) {
                    runner = [url, token, obsId, accessUrl, route](
                                     const QString &finder, const QJsonObject &params, std::atomic_bool *cancel,
                                     const std::function<void(const QString &)> &log) {
                        BackendClient client(url, token);
                        log(u"opening %1 on %2…"_s.arg(obsId, route.nodeCode));
                        const BackendOpenDatasetResult opened = client.openSkavaDataset(obsId, accessUrl);
                        SkavaFinderOutcome o;
                        if (!opened.valid) {
                            o.error = u"the node could not open %1: %2"_s.arg(
                                    obsId, opened.error.isEmpty() ? u"no detail"_s : opened.error);
                            return o;
                        }
                        client.setSessionId(opened.sessionId);
                        o = runFinderTask(client, opened.datasetId, finder, params, cancel, log);
                        o.node = route.nodeCode;
                        o.rankReason = route.rankReason;
                        o.backendUrl = url;
                        o.backendToken = token;
                        return o;
                    };
                } else {
                    // The worker outlives whatever the UI does: it gets a copy
                    // of the registry, never the Settings object.
                    const QList<BackendNode> registry = this->settings->getBackendNodes();
                    runner = [brokerUrl, skavaToken, obsId, registry](
                                     const QString &finder, const QJsonObject &params, std::atomic_bool *cancel,
                                     const std::function<void(const QString &)> &log) {
                        const BrokerJob job = BrokerClient(brokerUrl, skavaToken).run(obsId, finder, params, cancel, log);
                        SkavaFinderOutcome o;
                        o.finder = job.finder.isEmpty() ? finder : job.finder;
                        o.node = job.node;
                        o.rankReason = job.rankReason;
                        if (!job.valid || job.status != "COMPLETE"_L1) {
                            o.error = job.error.isEmpty() ? u"the broker's job ended as %1"_s.arg(job.status) : job.error;
                            return o;
                        }
                        o.valid = true;
                        o.nSources = job.nSources;
                        o.maskPath = job.maskPath;
                        o.catalogPath = job.catalogPath;
                        const BackendNode node = registeredBackendFor(registry, job.node, job.backend);
                        o.backendUrl = node.url;
                        o.backendToken = node.token;
                        return o;
                    };
                }
                // Through the broker the node is its choice, so the finders on
                // offer are the ones VisIVO backends ship (the broker places by
                // capability); directly, the node's own list.
                const QVector<BackendFinderInfo> offered = direct ? finders.finders : QVector<BackendFinderInfo>{};
                auto *dlg = new SkavaSourceFindDialog(offered, obsId, kind, where, runner, this);
                dlg->setAttribute(Qt::WA_DeleteOnClose);
                QObject::connect(dlg, &SkavaSourceFindDialog::openMaskRequested, this,
                                 [this, obsId](const SkavaFinderOutcome &o) {
                                     openBackendProductFits(o.backendUrl, o.backendToken, o.maskPath,
                                                            u"%1 [%2 mask on %3]"_s.arg(obsId, o.finder, o.node));
                                 });
                QObject::connect(dlg, &SkavaSourceFindDialog::openCatalogueRequested, this,
                                 [this](const SkavaFinderOutcome &o) {
                                     // The catalogue window reads the table through the
                                     // backend it is given: the node's, where the file is.
                                     auto *win = new vtkWindowCatalogue3D(o.catalogPath, o.backendUrl,
                                                                          o.backendToken, QStringList(), this);
                                     win->setAttribute(Qt::WA_DeleteOnClose);
                                     win->showMaximized();
                                     win->raise();
                                     win->activateWindow();
                                 });
                dlg->show();
                dlg->raise();
                dlg->activateWindow();
            });
}
