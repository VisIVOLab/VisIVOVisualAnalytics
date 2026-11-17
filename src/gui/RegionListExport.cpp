#include "RegionListExport.h"

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

#include <algorithm>

using namespace Qt::StringLiterals;

namespace visivo::regions {

void exportRegionList(QMainWindow *window, const RegionList &list, const ExportContext &ctx,
                      const std::vector<int> &only)
{
    if (!window)
        return;
    if (list.isEmpty()) {
        QMessageBox::information(window, u"Export Region"_s,
                                 u"Draw a region first (Tools ▸ Box/Circle/Polygon/Annulus)."_s);
        return;
    }
    // The dialogs below run their own event loops, and the window (with the
    // list) may close meanwhile.
    const QPointer<QMainWindow> guard(window);
    bool ok = false;
    // Which regions, by id: read again after the dialogs, as the list may
    // change while they are open.
    std::vector<int> ids;
    const auto visibleOf = [&list](const std::vector<int> &from) {
        std::vector<int> out;
        for (int id : from)
            if (const Region *r = list.find(id); r && r->visible)
                out.push_back(id);
        return out;
    };
    if (only.empty()) {
        for (const Region &r : list.regions())
            ids.push_back(r.id);
    } else {
        for (int id : only)
            if (list.find(id))
                ids.push_back(id);
    }
    const std::vector<int> shown = visibleOf(ids);
    const int hidden = int(ids.size() - shown.size());
    QStringList which;
    std::vector<std::vector<int>> picks;
    if (hidden > 0 && !shown.empty()) {
        which << u"The %1 visible region(s)"_s.arg(shown.size());
        picks.push_back(shown);
    }
    if (ids.size() > 1 || picks.empty()) {
        which << (hidden > 0 ? u"All %1 regions, the %2 hidden included"_s.arg(ids.size()).arg(hidden)
                             : u"All %1 regions"_s.arg(ids.size()));
        picks.push_back(ids);
    }
    if (only.empty() && list.count() > 1 && list.selected()) {
        which << u"The selected region (%1)"_s.arg(list.selected()->name);
        picks.push_back({ list.selectedId() });
    }
    // Asked whenever hidden regions would be written, so that is never by
    // surprise; not for the one region a row's Export… named.
    if (which.size() > 1 || (hidden > 0 && (only.empty() || ids.size() > 1))) {
        const QString w = QInputDialog::getItem(window, u"Export Region"_s, u"Regions to write:"_s,
                                                which, 0, false, &ok);
        if (!ok || !guard)
            return;
        ids = picks[std::size_t(std::max<qsizetype>(0, which.indexOf(w)))];
    } else {
        ids = picks.front();
    }
    const QStringList choices{
        u"DS9, sky (FK5)"_s, u"DS9, sky (the frame each region was typed in)"_s, u"DS9, pixel"_s,
        u"CASA CRTF, sky (FK5)"_s, u"CASA CRTF, sky (the frame each region was typed in)"_s,
        u"CASA CRTF, pixel"_s };
    const QString choice = QInputDialog::getItem(window, u"Export Region"_s,
                                                 u"Format and coordinate frame:"_s, choices, 0,
                                                 false, &ok);
    if (!ok || !guard)
        return;
    const bool isCrtf = choice.startsWith(u"CASA"_s);
    const bool ownFrames = choice.contains(u"each region"_s);
    const QString format = isCrtf ? u"crtf"_s : u"ds9"_s;
    const QString toFrame = choice.contains(u"sky"_s) ? u"fk5"_s : QString();
    const QString filter = isCrtf ? u"CRTF region (*.crtf)"_s : u"DS9 region (*.reg)"_s;
    const QString suffix = isCrtf ? u".crtf"_s : u".reg"_s;
    QString path = QFileDialog::getSaveFileName(window, u"Export Region File"_s,
                                                QFileInfo(ctx.filePath).baseName() + suffix, filter);
    if (path.isEmpty() || !guard)
        return;
    if (!path.endsWith(suffix))
        path += suffix;

    // 1-based pixels, as region files count them, and the frame each region
    // was typed or read in (FK5 for one drawn on the image). Taken after the
    // dialogs: the list may have changed while they were open.
    QJsonArray arr;
    QStringList frames;
    for (const auto &r : list.regions()) {
        if (std::find(ids.begin(), ids.end(), r.id) == ids.end())
            continue;
        arr.append(toCanonical(r, 1.0));
        frames << ((r.sourceFrame.isEmpty() || r.sourceFrame == u"image"_s) ? u"fk5"_s
                                                                             : r.sourceFrame);
    }
    if (arr.isEmpty()) {
        QMessageBox::information(window, u"Export Region"_s,
                                 u"The region to export was deleted meanwhile."_s);
        return;
    }
    const QString url = ctx.backendUrl, token = ctx.token, dsId = ctx.datasetId,
                  session = ctx.sessionId;
    const int n = arr.size();
    window->statusBar()->showMessage(u"Serializing %1 region(s)…"_s.arg(n), 0);
    const QPointer<QMainWindow> self(window);
    auto *watcher = new QFutureWatcher<BackendRegionExportResult>(window);
    QObject::connect(watcher, &QFutureWatcher<BackendRegionExportResult>::finished, window,
                     [self, watcher, path, n]() {
                         const BackendRegionExportResult r = watcher->result();
                         watcher->deleteLater();
                         if (!self)
                             return;
                         self->statusBar()->clearMessage();
                         if (!r.valid) {
                             QMessageBox::warning(self, u"Export Region"_s,
                                                  u"Export failed:\n%1"_s.arg(r.error));
                             return;
                         }
                         QFile out(path);
                         if (!out.open(QIODevice::WriteOnly | QIODevice::Text)) {
                             QMessageBox::warning(self, u"Export Region"_s,
                                                  u"Cannot write file:\n%1"_s.arg(path));
                             return;
                         }
                         out.write(r.text.toUtf8());
                         out.close();
                         QMessageBox::information(self, u"Export Region"_s,
                                                  u"Saved %1 region(s) to:\n%2"_s.arg(n).arg(path));
                     });
    watcher->setFuture(QtConcurrent::run([url, token, session, arr, format, dsId, toFrame, ownFrames,
                                          frames]() -> BackendRegionExportResult {
        BackendClient c(url, token);
        c.setSessionId(session);
        if (!ownFrames)
            return c.exportRegions(arr, format, toFrame.isEmpty() ? QString() : dsId, toFrame);
        // Each region back in its own frame first, through the dataset's WCS,
        // then written as it is.
        QJsonArray framed;
        for (int i = 0; i < arr.size(); ++i) {
            const BackendRegionConvertResult conv =
                    c.convertRegions(QJsonArray{ arr.at(i) }, frames.at(i), dsId, /*origin=*/1);
            if (!conv.valid || conv.regions.isEmpty()) {
                BackendRegionExportResult failed;
                failed.error = conv.error;
                return failed;
            }
            framed.append(conv.regions.first());
        }
        return c.exportRegions(framed, format);
    }));
}

}   // namespace visivo::regions
