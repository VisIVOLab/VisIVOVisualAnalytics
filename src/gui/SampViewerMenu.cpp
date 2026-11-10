#include "SampViewerMenu.h"

#include "MainWindow.h"
#include "app/BackendClient.h"

#include <QAction>
#include <QFutureWatcher>
#include <QtConcurrent>
#include <QApplication>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QPointer>

using namespace Qt::StringLiterals;

namespace visivo {

MainWindow *sampHost(const QWidget *viewer)
{
    // Viewers are usually parented to the Data Hub window; one opened
    // without a parent finds it among the top-level windows.
    for (const QWidget *w = viewer; w; w = w->parentWidget()) {
        if (auto *mw = qobject_cast<MainWindow *>(const_cast<QWidget *>(w)))
            return mw;
    }
    for (QWidget *w : QApplication::topLevelWidgets()) {
        if (auto *mw = qobject_cast<MainWindow *>(w))
            return mw;
    }
    return nullptr;
}

bool addSampConnectionLine(QMenu *menu, MainWindow *host)
{
    if (host && host->sampConnected())
        return true;
    QAction *state = menu->addAction(u"Not connected to a SAMP hub"_s);
    state->setEnabled(false);
    QAction *connectAction = menu->addAction(u"Connect To Hub"_s);
    connectAction->setToolTip(
            u"Join the SAMP hub that TOPCAT, Aladin or DS9 started (the Data Hub's "
            u"SAMP menu shows the connection)."_s);
    connectAction->setEnabled(host != nullptr);
    QPointer<MainWindow> target = host;
    QObject::connect(connectAction, &QAction::triggered, menu, [target]() {
        if (target)
            target->connectSampHub();
    });
    menu->addSeparator();
    return false;
}

void addSampSendTo(QMenu *menu, MainWindow *host, const QString &title, const QString &mtype,
                   const QString &unavailable, std::function<void(const QString &)> send)
{
    QString why = unavailable;
    if (why.isEmpty() && (!host || !host->sampConnected()))
        why = u"Connect to a SAMP hub first."_s;
    if (!why.isEmpty()) {
        QAction *off = menu->addAction(title);
        off->setEnabled(false);
        off->setToolTip(why);
        // A disabled entry's tooltip is not shown on every platform: say it
        // in the entry's status tip too (the status bar).
        off->setStatusTip(why);
        return;
    }
    QMenu *sub = menu->addMenu(title);
    sub->setToolTipsVisible(true);
    QAction *all = sub->addAction(u"All Tools"_s);
    all->setToolTip(u"Every running tool that takes %1."_s.arg(mtype));
    QObject::connect(all, &QAction::triggered, menu, [send]() { send(QString()); });
    const QList<Samp::Recipient> recipients = host->sampRecipients(mtype);
    sub->addSeparator();
    if (recipients.isEmpty()) {
        // The list is the hub's as last reported; All Tools asks the hub
        // again, and says so if nobody takes it.
        QAction *none = sub->addAction(u"No listed tool takes %1"_s.arg(mtype));
        none->setEnabled(false);
        return;
    }
    for (const Samp::Recipient &r : recipients) {
        QAction *one = sub->addAction(r.label);
        const QString id = r.id;
        QObject::connect(one, &QAction::triggered, menu, [send, id]() { send(id); });
    }
}

QMenu *installSampMenu(QMainWindow *window, std::function<void(QMenu *, MainWindow *)> fill)
{
    auto *menu = window->menuBar()->addMenu(u"SAMP"_s);
    menu->setToolTipsVisible(true);
    QPointer<QMainWindow> owner = window;
    QObject::connect(menu, &QMenu::aboutToShow, menu, [menu, owner, fill]() {
        // The submenus of the last opening are children of the menu, not
        // owned by its actions: clear() alone would keep every one of them.
        for (QMenu *old : menu->findChildren<QMenu *>(Qt::FindDirectChildrenOnly))
            old->deleteLater();
        menu->clear();
        if (!owner)
            return;
        MainWindow *host = sampHost(owner);
        addSampConnectionLine(menu, host);
        fill(menu, host);
    });
    return menu;
}

void resolvePixelSky(QObject *context, const PixelSkyRequest &request,
                     std::function<std::optional<samp::SkyPosition>()> fallback,
                     std::function<void(const PixelSky &)> done)
{
    auto *watcher = new QFutureWatcher<BackendPixelSkyResult>(context);
    QObject::connect(watcher, &QFutureWatcher<BackendPixelSkyResult>::finished, context,
                     [watcher, fallback, done]() {
                         watcher->deleteLater();
                         const BackendPixelSkyResult r = watcher->result();
                         PixelSky out;
                         if (r.valid) {
                             out.position = samp::SkyPosition{ r.raDeg, r.decDeg };
                         } else if (r.routeMissing) {
                             // An older backend: the header's linear axes and
                             // projection, without rotation or distortion.
                             out.local = true;
                             out.position = fallback();
                             if (!out.position)
                                 out.problem = u"the image has no sky coordinates there"_s;
                         } else {
                             out.problem = r.error.isEmpty() ? u"the backend did not say where"_s
                                                             : r.error;
                         }
                         done(out);
                     });
    watcher->setFuture(QtConcurrent::run([request]() {
        BackendClient client(request.backendUrl, request.token);
        client.setSessionId(request.sessionId);
        return client.pixelToSky(request.datasetId, request.x, request.y, request.z);
    }));
}

}  // namespace visivo
