#include "RegionImport.h"

#include "app/BackendClient.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QMap>
#include <QHeaderView>
#include <QLabel>
#include <QMainWindow>
#include <QMessageBox>
#include <QPointer>
#include <QStatusBar>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QtConcurrentRun>

using namespace Qt::StringLiterals;

namespace visivo::regions {

void importRegionFile(QMainWindow *window, const ExportContext &ctx,
                      std::function<void(const ImportResult &)> apply, const QString &givenPath,
                      const QString &note)
{
    if (!window)
        return;
    const QPointer<QMainWindow> guard(window);
    QString path = givenPath;
    if (path.isEmpty())
        path = QFileDialog::getOpenFileName(
                window, u"Import Region File"_s, QFileInfo(ctx.filePath).absolutePath(),
                u"Region files (*.reg *.crtf);;DS9 (*.reg);;CRTF (*.crtf);;All files (*)"_s);
    if (path.isEmpty() || !guard)
        return;
    QFile in(path);
    if (!in.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QMessageBox::warning(window, u"Import Region"_s, u"Cannot open file:\n%1"_s.arg(path));
        return;
    }
    const QString text = QString::fromUtf8(in.readAll());
    in.close();

    const QString url = ctx.backendUrl, token = ctx.token, session = ctx.sessionId,
                  dsId = ctx.datasetId;
    const QString fileName = QFileInfo(path).fileName();
    window->statusBar()->showMessage(u"Reading %1…"_s.arg(fileName), 0);
    auto *watcher = new QFutureWatcher<BackendRegionParseResult>(window);
    QObject::connect(
            watcher, &QFutureWatcher<BackendRegionParseResult>::finished, window,
            [guard, watcher, apply, text, fileName, note]() {
                const BackendRegionParseResult r = watcher->result();
                watcher->deleteLater();
                if (!guard)
                    return;
                guard->statusBar()->clearMessage();
                // A response with nothing read still carries the reasons; only a
                // failed request has nothing to show but its error.
                if (!r.valid && r.diagnostics.isEmpty()) {
                    QMessageBox::warning(guard, u"Import Region"_s,
                                         u"%1 could not be read:\n%2"_s.arg(fileName, r.error));
                    return;
                }
                ImportResult result = importFromParse(r.regions, r.diagnostics,
                                                      r.diagnosticsTruncated, r.skipped, text);
                result.fileName = fileName;
                if (!result.regions.empty())
                    apply(result);
                if (!guard)
                    return;
                guard->statusBar()->showMessage(
                        u"%1: %2%3"_s.arg(fileName, importSummary(result),
                                          note.isEmpty() ? QString() : u" "_s + note),
                        10000);
                if (!result.diagnostics.empty() || result.regions.empty())
                    showImportReport(guard, fileName, result);
            });
    // In image pixels through the dataset's WCS, so every frame and every
    // coordinate format lands where the dataset puts it.
    watcher->setFuture(QtConcurrent::run([url, token, session, text, dsId]() {
        BackendClient c(url, token);
        c.setSessionId(session);
        return c.parseRegions(text, u"auto"_s, dsId, u"image"_s);
    }));
}

namespace {

/// Each of @p in moved to @p toFrame by @p client with the dataset's WCS: in one
/// call, and when the backend refuses that (one region off the projection fails
/// the lot) region by region, so the others still arrive. @p index has the
/// position in @p in of each region in @p out; @p reasons, by position in @p in,
/// why the others did not go.
void convertEach(const BackendClient &client, const QJsonArray &in, const QString &toFrame,
                 const QString &datasetId, QVector<int> &index, QJsonArray &out,
                 QMap<int, QString> &reasons)
{
    if (in.size() > 1) {
        const BackendRegionConvertResult r = client.convertRegions(in, toFrame, datasetId, 1);
        if (r.valid && r.regions.size() == in.size()) {
            for (int i = 0; i < in.size(); ++i)
                index.append(i);
            out = r.regions;
            return;
        }
        // Only a region off the projection is worth trying one by one; a backend
        // that is down or a dataset with no sky would fail them all again.
        if (!r.error.contains(u"projection"_s, Qt::CaseInsensitive)) {
            for (int i = 0; i < in.size(); ++i)
                reasons.insert(i, r.error);
            return;
        }
    }
    for (int i = 0; i < in.size(); ++i) {
        const BackendRegionConvertResult r =
                client.convertRegions(QJsonArray{ in.at(i) }, toFrame, datasetId, 1);
        if (r.valid && r.regions.size() == 1) {
            index.append(i);
            out.append(r.regions.first());
        } else {
            reasons.insert(i, r.error);
        }
    }
}

}   // namespace

void sendRegionsToDataset(QMainWindow *window, const std::vector<Region> &regions,
                          const ExportContext &from, const ExportContext &to, const QString &label,
                          std::function<void(const ImportResult &)> apply)
{
    if (!window || regions.empty())
        return;
    QJsonArray canonical;
    for (const Region &r : regions)
        canonical.append(toCanonical(r, 1.0));
    const QPointer<QMainWindow> guard(window);
    window->statusBar()->showMessage(u"Placing %1 region(s) on the sky…"_s.arg(regions.size()), 0);
    auto *watcher = new QFutureWatcher<ImportResult>(window);
    QObject::connect(watcher, &QFutureWatcher<ImportResult>::finished, window,
                     [guard, watcher, apply]() {
                         const ImportResult result = watcher->result();
                         watcher->deleteLater();
                         if (!guard)
                             return;
                         guard->statusBar()->clearMessage();
                         if (!result.regions.empty())
                             apply(result);
                         if (guard && (!result.diagnostics.empty() || result.regions.empty()))
                             showImportReport(guard, result.fileName, result);
                     });
    watcher->setFuture(QtConcurrent::run([regions, canonical, from, to, label]() {
        BackendClient a(from.backendUrl, from.token), b(to.backendUrl, to.token);
        a.setSessionId(from.sessionId);
        b.setSessionId(to.sessionId);
        // Pixels to the sky on the first dataset, the sky to pixels on the second.
        QVector<int> onSky, placed;
        QJsonArray sky, pixels;
        QMap<int, QString> reasons;   // by position in `regions`
        convertEach(a, canonical, u"icrs"_s, from.datasetId, onSky, sky, reasons);
        QMap<int, QString> offSecond;   // by position in `sky`
        convertEach(b, sky, u"image"_s, to.datasetId, placed, pixels, offSecond);
        for (auto it = offSecond.cbegin(); it != offSecond.cend(); ++it)
            reasons.insert(onSky.at(it.key()), it.value());

        ImportResult result;
        result.fileName = label;
        for (int k = 0; k < pixels.size(); ++k) {
            const Region &src = regions.at(static_cast<std::size_t>(onSky.at(placed.at(k))));
            // One at a time, so each stays tied to the region it came from.
            ImportResult one = importFromParse(QJsonArray{ pixels.at(k) }, {}, false, 0, QString());
            if (one.regions.empty()) {
                reasons.insert(onSky.at(placed.at(k)),
                               one.diagnostics.empty() ? QString() : one.diagnostics.front().reason);
                continue;
            }
            // What the backend does not carry: the name, colour and file properties.
            Region r = one.regions.front();
            r.name = src.name;
            r.color = src.color;
            r.props = src.props;
            r.sourceLine = src.sourceLine;
            if (src.shape == Shape::Text)
                r.text = src.text;
            result.regions.push_back(r);
        }
        for (auto it = reasons.cbegin(); it != reasons.cend(); ++it) {
            const Region &src = regions.at(static_cast<std::size_t>(it.key()));
            result.diagnostics.push_back(
                    { 0, src.name.isEmpty() ? shapeLabel(src.shape) : src.name,
                      it.value().isEmpty() ? u"it is not on the sky the other image covers"_s
                                           : it.value() });
            ++result.rejectedHere;
        }
        return result;
    }));
}

void showImportReport(QWidget *parent, const QString &fileName, const ImportResult &result)
{
    auto *dlg = new QDialog(parent);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    // A tool window: it stays above the viewer while the user looks at the
    // regions it describes, and does not block the window (non-modal).
    dlg->setWindowFlag(Qt::Tool);
    dlg->setWindowTitle(u"Region Import: %1"_s.arg(fileName));
    auto *layout = new QVBoxLayout(dlg);
    const int skipped = result.notRead();
    QString summary = u"%1 region(s) read; %2 line(s) not read."_s.arg(result.regions.size())
                              .arg(skipped);
    if (result.truncated)
        summary += u" Only the first %1 lines the backend skipped are listed."_s.arg(
                int(result.diagnostics.size()) - result.rejectedHere);
    auto *head = new QLabel(summary, dlg);
    head->setWordWrap(true);
    layout->addWidget(head);
    auto *table = new QTableWidget(int(result.diagnostics.size()), 3, dlg);
    table->setHorizontalHeaderLabels({ u"Line"_s, u"Text"_s, u"Reason"_s });
    table->verticalHeader()->hide();
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setWordWrap(true);
    for (int i = 0; i < int(result.diagnostics.size()); ++i) {
        const ImportDiagnostic &d = result.diagnostics[std::size_t(i)];
        table->setItem(i, 0, new QTableWidgetItem(d.line > 0 ? QString::number(d.line) : u"–"_s));
        table->setItem(i, 1, new QTableWidgetItem(d.text));
        table->setItem(i, 2, new QTableWidgetItem(d.reason));
        table->item(i, 1)->setToolTip(d.text);
        table->item(i, 2)->setToolTip(d.reason);
    }
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Interactive);
    table->horizontalHeader()->setStretchLastSection(true);
    table->setColumnWidth(1, 260);
    table->resizeRowsToContents();
    layout->addWidget(table, 1);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, dlg);
    QObject::connect(buttons, &QDialogButtonBox::rejected, dlg, &QDialog::close);
    layout->addWidget(buttons);
    dlg->resize(720, 360);
    dlg->show();
    dlg->raise();
}

}   // namespace visivo::regions
