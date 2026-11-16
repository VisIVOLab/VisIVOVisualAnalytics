#include "RegionImport.h"

#include "app/BackendClient.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
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
                const ImportResult result = importFromParse(r.regions, r.diagnostics,
                                                            r.diagnosticsTruncated, r.skipped, text);
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
