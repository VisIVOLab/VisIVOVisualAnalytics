#include "ContourExport.h"

#include "app/BackendClient.h"

#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QInputDialog>
#include <QJsonArray>
#include <QMainWindow>
#include <QMessageBox>
#include <QPointer>
#include <QStatusBar>
#include <QtConcurrentRun>

using namespace Qt::StringLiterals;

namespace visivo::contours {

namespace {

struct Written
{
    bool valid{ false };
    QString error;
    QString text;
};

QString frameLabel(const QString &frame)
{
    if (frame == u"galactic")
        return u"Galactic"_s;
    if (frame == u"ecliptic")
        return u"Ecliptic"_s;
    return u"FK5 (RA, Dec)"_s;
}

}   // namespace

void exportContours(QMainWindow *window, const std::vector<Line> &lines, double lineWidth,
                    const visivo::regions::ExportContext &ctx, const QString &defaultFrame)
{
    if (!window)
        return;
    if (lines.empty()) {
        QMessageBox::information(window, u"Export Contours"_s,
                                 u"There are no contour lines to export: show the contours first, "
                                 u"with levels that cross the image."_s);
        return;
    }
    // The dialogs run their own event loops, and the window may close meanwhile.
    const QPointer<QMainWindow> guard(window);
    bool ok = false;
    const QString kind = QInputDialog::getItem(
            window, u"Export Contours"_s, u"File:"_s,
            { u"DS9 region file"_s, u"Table of vertices (CSV)"_s }, 0, false, &ok);
    if (!ok || !guard)
        return;
    const bool isCsv = kind.startsWith(u"Table");

    QStringList frames;      // what the user sees
    QStringList frameKeys;   // what the backend is told; empty = pixels
    if (!ctx.datasetId.isEmpty()) {
        QStringList order{ defaultFrame, u"galactic"_s, u"fk5"_s, u"ecliptic"_s };
        order.removeDuplicates();
        for (const QString &f : std::as_const(order)) {
            frames << u"Sky, %1"_s.arg(frameLabel(f));
            frameKeys << f;
        }
    }
    frames << u"Image pixels (1-based)"_s;
    frameKeys << QString();
    const QString frameChoice = frames.size() == 1
            ? frames.first()
            : QInputDialog::getItem(window, u"Export Contours"_s, u"Coordinates:"_s, frames, 0,
                                    false, &ok);
    if (frames.size() > 1 && (!ok || !guard))
        return;
    const QString toFrame = frameKeys.value(frames.indexOf(frameChoice));

    const QString suffix = isCsv ? u".csv"_s : u".reg"_s;
    QString path = QFileDialog::getSaveFileName(
            window, u"Export Contours"_s,
            QFileInfo(ctx.filePath).baseName() + u"_contours"_s + suffix,
            isCsv ? u"CSV table (*.csv)"_s : u"DS9 region (*.reg)"_s);
    if (path.isEmpty() || !guard)
        return;
    if (!path.endsWith(suffix))
        path += suffix;

    const QJsonArray regions = canonicalRegions(lines, lineWidth);
    if (regions.isEmpty()) {
        QMessageBox::information(window, u"Export Contours"_s,
                                 u"The contour lines are too short to be written."_s);
        return;
    }
    const QString url = ctx.backendUrl, token = ctx.token, session = ctx.sessionId,
                  dataset = ctx.datasetId;
    const int n = regions.size();
    window->statusBar()->showMessage(u"Writing %1 contour line(s)…"_s.arg(n), 0);
    auto *watcher = new QFutureWatcher<Written>(window);
    const QPointer<QMainWindow> self(window);
    QObject::connect(watcher, &QFutureWatcher<Written>::finished, window,
                     [self, watcher, path, n]() {
                         const Written r = watcher->result();
                         watcher->deleteLater();
                         if (!self)
                             return;
                         self->statusBar()->clearMessage();
                         if (!r.valid) {
                             QMessageBox::warning(self, u"Export Contours"_s,
                                                  u"Export failed:\n%1"_s.arg(r.error));
                             return;
                         }
                         QFile out(path);
                         if (!out.open(QIODevice::WriteOnly | QIODevice::Text)) {
                             QMessageBox::warning(self, u"Export Contours"_s,
                                                  u"Cannot write file:\n%1"_s.arg(path));
                             return;
                         }
                         out.write(r.text.toUtf8());
                         out.close();
                         QMessageBox::information(self, u"Export Contours"_s,
                                                  u"Saved %1 contour line(s) to:\n%2"_s.arg(n).arg(path));
                     });
    watcher->setFuture(QtConcurrent::run([=]() -> Written {
        Written w;
        BackendClient c(url, token);
        c.setSessionId(session);
        if (isCsv) {
            if (toFrame.isEmpty()) {
                w.text = verticesCsv(regions, u"x_pix"_s, u"y_pix"_s);
            } else {
                const BackendRegionConvertResult conv =
                        c.convertRegions(regions, toFrame, dataset, /*origin=*/1);
                if (!conv.valid) {
                    w.error = conv.error;
                    return w;
                }
                const bool galactic = toFrame == u"galactic", ecliptic = toFrame == u"ecliptic";
                w.text = verticesCsv(conv.regions,
                                     galactic ? u"glon_deg"_s : ecliptic ? u"elon_deg"_s : u"ra_deg"_s,
                                     galactic ? u"glat_deg"_s : ecliptic ? u"elat_deg"_s : u"dec_deg"_s);
            }
            w.valid = true;
            return w;
        }
        const BackendRegionExportResult r =
                c.exportRegions(regions, u"ds9"_s, toFrame.isEmpty() ? QString() : dataset, toFrame);
        w.valid = r.valid;
        w.error = r.error;
        w.text = r.text;
        return w;
    }));
}

}   // namespace visivo::contours
