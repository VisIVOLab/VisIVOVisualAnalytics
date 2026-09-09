#include "MainWindow.h"
#include <QHash>
#include <functional>
#include "ui_MainWindow.h"

#include "AboutDialog.h"
#include "AuthWrapper.h"
#include "app/BackendClient.h"
#include "app/BackendClientFactory.h"
#include "app/SampClient.h"
#include "app/SampMessageHandler.h"
#include "app/RecentDatasetsManager.h"
#include "app/ThemeManager.h"
#include "BaselineDialog.h"
#include "CommandPalette.h"
#include "DataHubWidget.h"
#include "DiagnosticsWindow.h"
#include "LinewidthDialog.h"
#include "RemoteFileBrowserDialog.h"
#include "Settings.h"
#include "SettingsDialog.h"
#include "theme/VisivoTheme.h"
#include "SourceFindDialog.h"
#include "StackDialog.h"
#include "WebViewProcess.h"
#include "vtkWindowCatalogue3D.h"
#include "vtkWindowCube.h"
#include "vtkWindowImage.h"

#include <QLabel>
#include <QStatusBar>
#include "HiPSWindow.h"
#include "vtkWindowVbt.h"
#include "vtkWindowVbtVolume.h"
#include "VLKBInventoryTree.h"
#include "SkavaSearchPanel.h"
#include "../skava/SkavaClient.h"
#include "../Settings.h"
#include "vlkb/VlkbQueryService.h"

#include <QButtonGroup>
#include <QCloseEvent>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QDoubleValidator>
#include <QFileDialog>
#include <QApplication>
#include <QCoreApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QMessageBox>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QTabBar>
#include <QTabWidget>
#include <QWebChannel>
#include <QPixmap>
#include <QStyle>
#include <QPainter>
#include <QUrl>
#include <QtConcurrent>

#include <cmath>
#include <memory>

#include "app/DiagnosticsManager.h"

#include <vtkImageData.h>

using namespace Qt::StringLiterals;

// ── MainWindow: Menus (extracted from MainWindow.cpp) ──

void MainWindow::openCommandPalette()
{
    if (!this->m_commandPalette) {
        this->m_commandPalette = new CommandPalette(this);
    }
    this->rebuildCommandPalette();
    this->m_commandPalette->show();
    this->m_commandPalette->raise();
    this->m_commandPalette->activateWindow();
}


void MainWindow::rebuildCommandPalette()
{
    if (!this->m_commandPalette) return;

    // Static actions are added once; dynamic ones (Recent datasets) get
    // refreshed each call by clearing their group first.
    static bool s_staticAdded = false;
    if (!s_staticAdded) {
        // ── File ────────────────────────────────────────────────────────────
        this->m_commandPalette->addAction(
                // ▣ and not ⌘: the Command symbol reads as a keyboard modifier,
                // and the shortcut column two inches to the right is full of
                // real ones. Same reason ⌥ is gone from Spectral Stacking.
                {u"▣"_s, u"Open Remote Dataset…"_s,
                 u"File · backend filesystem · FITS"_s, u"⌘O"_s, u"File"_s,
                 [this]() { this->openData(); }});
        this->m_commandPalette->addAction(
                {u"✦"_s, u"Open 3D Catalogue…"_s,
                 u"File · CSV / VOTable / FITS / speck / IPAC in 3D viewer"_s, QString(), u"File"_s,
                 [this]() { this->openCatalogue3D(); }});
        this->m_commandPalette->addAction(
                {u"▦"_s, u"Open VBT…"_s,
                 u"File · VisIVO Binary Table point-cloud"_s, QString(), u"File"_s,
                 [this]() { this->openVbt(); }});
        this->m_commandPalette->addAction(
                {u"◐"_s, u"HiPS Viewer…"_s,
                 u"File · hierarchical sky surveys"_s, QString(), u"File"_s,
                 [this]() { this->openHiPS(); }});
        this->m_commandPalette->addAction(
                {u"◑"_s, u"VLKB Inventory"_s,
                 u"File · browse remote VLKB sources"_s, QString(), u"File"_s,
                 [this]() { this->revealVLKBTab(); }});

        // ── Application ─────────────────────────────────────────────────────
        this->m_commandPalette->addAction(
                {u"⚙"_s, u"Open Settings…"_s,
                 u"Edit · backend URL, theme, token"_s, u"⌘,"_s,
                 u"Application"_s,
                 [this]() { this->openSettingsDialog(); }});
        this->m_commandPalette->addAction(
                {u"ⓘ"_s, u"About VisIVO Visual Analytics"_s,
                 u"Version, credits"_s, QString(), u"Application"_s,
                 [this]() { this->openAboutDialog(); }});
        this->m_commandPalette->addAction(
                {u"⏻"_s, u"Quit VisIVO Visual Analytics"_s,
                 u"Close the application"_s, u"⌘Q"_s, u"Application"_s,
                 [this]() { this->close(); }});

        // ── View ────────────────────────────────────────────────────────────
        // Diagnostics is in MainWindow's View menu but easy to miss; expose it
        // in the palette so it's reachable from anywhere via ⌘K + "diag".
        this->m_commandPalette->addAction(
                {u"⚠"_s, u"Open Diagnostics Window"_s,
                 u"View · backend / SAMP / scientific log feed"_s, QString(),
                 u"View"_s,
                 [this]() { this->openDiagnosticsWindow(); }});

        // ── Science ─────────────────────────────────────────────────────────
        // Each entry calls QAction::trigger(), which respects the action's
        // enabled state (disabled actions silently no-op when no dataset is
        // open). The menubar's Science menu shows the actual enabled state.
        this->m_commandPalette->addAction(
                {u"∿"_s, u"Line-Width Map…"_s,
                 u"Science · FWHM / equivalent-width per pixel"_s,
                 QString(), u"Science"_s, [this]() {
                     if (this->m_actLinewidth) this->m_actLinewidth->trigger();
                 }});
        this->m_commandPalette->addAction(
                {u"⌇"_s, u"Baseline Subtraction…"_s,
                 u"Science · polynomial or median spectral baseline"_s,
                 QString(), u"Science"_s, [this]() {
                     if (this->m_actBaseline) this->m_actBaseline->trigger();
                 }});
        this->m_commandPalette->addAction(
                {u"≡"_s, u"Spectral Stacking…"_s,
                 u"Science · stack multiple session cubes into 1-D spectrum"_s,
                 QString(), u"Science"_s, [this]() {
                     if (this->m_actStack) this->m_actStack->trigger();
                 }});
        this->m_commandPalette->addAction(
                {u"⌖"_s, u"Source Finding (SoFiA-2)…"_s,
                 u"Science · run SoFiA-2 on the current dataset"_s,
                 QString(), u"Science"_s, [this]() {
                     if (this->m_actSourceFind) this->m_actSourceFind->trigger();
                 }});

        // ── SAMP ────────────────────────────────────────────────────────────
        // Cross-application messaging hub. Actions delegate to the QAction
        // toggled in buildSampMenu(); the menu mirror surfaces hub state.
        this->m_commandPalette->addAction(
                {u"⇄"_s, u"SAMP: Connect to Hub"_s,
                 u"SAMP · join the running hub"_s,
                 QString(), u"SAMP"_s, [this]() {
                     if (this->m_actSampConnect) this->m_actSampConnect->trigger();
                 }});
        this->m_commandPalette->addAction(
                {u"⇎"_s, u"SAMP: Disconnect from Hub"_s,
                 u"SAMP · leave the hub"_s,
                 QString(), u"SAMP"_s, [this]() {
                     if (this->m_actSampDisconnect) this->m_actSampDisconnect->trigger();
                 }});
        this->m_commandPalette->addAction(
                {u"↗"_s, u"SAMP: Send Current FITS Dataset"_s,
                 u"SAMP · broadcast current cube/image to peers"_s,
                 QString(), u"SAMP"_s, [this]() {
                     if (this->m_actSampSendCurrentFits)
                         this->m_actSampSendCurrentFits->trigger();
                 }});
        this->m_commandPalette->addAction(
                {u"↗"_s, u"SAMP: Send Current Catalogue"_s,
                 u"SAMP · broadcast current catalogue to peers"_s,
                 QString(), u"SAMP"_s, [this]() {
                     if (this->m_actSampSendCurrentCatalogue)
                         this->m_actSampSendCurrentCatalogue->trigger();
                 }});

        s_staticAdded = true;
    }

    // ── The active window's own menus (dynamic) ─────────────────────────────
    //
    // ⌘K from a viewer forwards here, so before this the palette answered with
    // MainWindow's commands only: pressing it in the cube viewer offered "Open
    // Remote Dataset" and nothing you could actually do to the cube. Everything
    // that window can do already exists, in its menu bar — so read it, rather
    // than curate a second list that would drift the first time someone adds a
    // menu item and forgets the palette.
    //
    // The source is whatever window is active when the palette is asked for.
    // This runs from a queued invocation, before the palette is shown, so the
    // viewer that invoked it is still the active window.
    for (const QString &g : std::as_const(this->m_paletteWindowGroups))
        this->m_commandPalette->clearGroup(g);
    this->m_paletteWindowGroups.clear();

    if (auto *src = qobject_cast<QMainWindow *>(QApplication::activeWindow())) {
        if (src != this && src->menuBar()) {
            // A glyph per menu, so the viewer's entries read as a list rather
            // than eighty rows of the same "›" fallback next to the curated
            // entries that all have one. Keyed on the menu title, with "›" for
            // anything a future menu adds. Every character here was checked
            // against the app font — none falls back to a substitute family,
            // which is what makes a glyph column line up.
            static const QHash<QString, QString> kMenuGlyph{
                { u"Camera"_s, u"\u25ce"_s },   // ◎
                { u"Edit"_s,   u"\u270e"_s },   // ✎
                { u"View"_s,   u"\u25f1"_s },   // ◱
                { u"WCS"_s,    u"\u2295"_s },   // ⊕
                { u"Tools"_s,  u"\u2692"_s },   // ⚒
                { u"File"_s,   u"\u25a3"_s },   // ▣
                { u"Data"_s,   u"\u25a3"_s },
            };
            // Depth-first over the menu tree; only leaves become commands, and a
            // submenu's title joins the group so "Camera ▸ Presets" stays legible.
            const std::function<void(QMenu *, const QString &)> walk =
                    [&](QMenu *menu, const QString &group) {
                        if (!menu)
                            return;
                        const auto acts = menu->actions();
                        for (QAction *a : acts) {
                            if (!a || a->isSeparator())
                                continue;
                            if (a->menu()) {
                                walk(a->menu(), group + u" \u25b8 "_s + a->menu()->title()
                                                                 .remove(u'&'));
                                continue;
                            }
                            const QString label = a->text().remove(u'&');
                            if (label.isEmpty())
                                continue;
                            // Checkable entries say which way the command will
                            // flip them; a bare "Show Cutting Plane" in a list is
                            // ambiguous about what pressing it does. A disabled
                            // one says so rather than looking available and doing
                            // nothing when chosen. And QAction::toolTip() falls
                            // back to the action's own text, so an entry with no
                            // tooltip would otherwise print its label twice.
                            QString subtitle;
                            if (!a->isEnabled())
                                subtitle = u"Unavailable right now"_s;
                            else if (a->isCheckable())
                                subtitle = a->isChecked() ? u"On \u2014 select to turn off"_s
                                                          : u"Off \u2014 select to turn on"_s;
                            else {
                                const QString tip = a->toolTip().remove(u'&');
                                if (tip != label)
                                    subtitle = tip;
                            }
                            if (!this->m_paletteWindowGroups.contains(group))
                                this->m_paletteWindowGroups.append(group);
                            // The top-level menu's glyph, not the submenu's:
                            // "View ▸ Animation Speed" belongs to View.
                            const QString glyph = kMenuGlyph.value(
                                    group.section(u" \u25b8 "_s, 0, 0)
                                            .section(u" \u00b7 "_s, -1));
                            this->m_commandPalette->addAction(
                                    { glyph, label, subtitle,
                                      a->shortcut().toString(QKeySequence::NativeText), group,
                                      [a]() { if (a && a->isEnabled()) a->trigger(); } });
                        }
                    };
            // Prefixed, and not by decoration: the viewer's menu titles collide
            // with the curated groups — a cube contributes "View", an image
            // viewer "File" and "View" — and the cleanup below clears groups BY
            // NAME. Opening the palette from a viewer and reopening it therefore
            // deleted the permanent "Open Diagnostics Window" and the File
            // commands, with s_staticAdded making sure they never came back.
            // The prefix also says what the reader needs to know: these act on
            // the window in front of them, not on the application.
            const auto top = src->menuBar()->actions();
            for (QAction *a : top)
                if (a->menu())
                    walk(a->menu(),
                         u"This window \u00b7 "_s + a->menu()->title().remove(u'&'));
        }
    }

    // ── Recent datasets (dynamic) ───────────────────────────────────────────
    this->m_commandPalette->clearGroup(u"Recent datasets"_s);
    if (this->recentMgr) {
        const auto recent = this->recentMgr->datasets();
        // Cap at 6 to keep the palette tidy.
        const int limit = std::min<int>(6, recent.size());
        for (int i = 0; i < limit; ++i) {
            const RecentDataset ds = recent[i];
            const QString sub =
                    ds.type.isEmpty()
                            ? u"remote"_s
                            : QStringLiteral("%1 · remote").arg(ds.type.toUpper());
            this->m_commandPalette->addAction(
                    {u"▣"_s, ds.displayName, sub, QString(),
                     u"Recent datasets"_s,
                     [this, ds]() { this->reopenDataset(ds); }});
        }
    }
}


void MainWindow::buildDataMenu()
{
    // Insert "Data" menu between File and Edit so it is the primary navigation.
    auto *dataMenu = new QMenu(u"Data"_s, this);
    this->menuBar()->insertMenu(ui->menuEdit->menuAction(), dataMenu);

    // Shortcuts are already defined on the File-menu actions in the .ui file;
    // do not duplicate them here to avoid ambiguous shortcut warnings.
    auto *actOpenData = dataMenu->addAction(u"Open Remote Dataset…"_s);
    QObject::connect(actOpenData, &QAction::triggered, this, &MainWindow::openData);

    auto *actOpenCatalogue = dataMenu->addAction(u"Open 3D Catalogue (CSV)…"_s);
    QObject::connect(actOpenCatalogue, &QAction::triggered, this, &MainWindow::openCatalogue3D);

    auto *actOpenVbt = dataMenu->addAction(u"Open VBT…"_s);
    QObject::connect(actOpenVbt, &QAction::triggered, this, &MainWindow::openVbt);

    auto *actOpenVelocity = dataMenu->addAction(u"Open Velocity Field…"_s);
    actOpenVelocity->setToolTip(
            u"Open a cosmic-flow velocity field (3-component FITS) as 3-D streamlines / arrows."_s);
    QObject::connect(actOpenVelocity, &QAction::triggered, this, &MainWindow::openVelocityField);

    dataMenu->addSeparator();

    auto *actHiPS = dataMenu->addAction(u"HiPS Viewer…"_s);
    actHiPS->setToolTip(u"Open the HiPS panoramic sky viewer."_s);
    QObject::connect(actHiPS, &QAction::triggered, this, &MainWindow::openHiPS);
}


void MainWindow::buildSampMenu()
{
    auto *sampMenu = new QMenu(u"SAMP"_s, this);
    this->menuBar()->insertMenu(ui->menuHelp->menuAction(), sampMenu);
    m_sampMenu = sampMenu;

    m_actSampConnect = sampMenu->addAction(u"Connect To Hub"_s);
    QObject::connect(m_actSampConnect, &QAction::triggered, this, [this]() {
        this->syncSampSettings();
        const auto health = this->m_sampBackendClient->health();
        if (!health.ok) {
            QMessageBox::critical(
                    this, u"Backend unavailable"_s,
                    u"Cannot connect SAMP integration because the backend is not reachable.\n\n"
                    u"Error: %1"_s.arg(health.error.isEmpty() ? u"No response"_s : health.error));
            return;
        }
        this->m_sampClient->connectToHub();
    });

    m_actSampDisconnect = sampMenu->addAction(u"Disconnect From Hub"_s);
    QObject::connect(m_actSampDisconnect, &QAction::triggered, this, [this]() {
        this->m_sampClient->disconnectFromHub();
    });

    sampMenu->addSeparator();

    m_actSampSendCurrentFits = sampMenu->addAction(u"Send Current FITS Dataset"_s);
    QObject::connect(m_actSampSendCurrentFits, &QAction::triggered,
                     this, &MainWindow::sendCurrentFitsViaSamp);

    m_actSampSendCurrentCatalogue = sampMenu->addAction(u"Send Current Catalogue"_s);
    QObject::connect(m_actSampSendCurrentCatalogue, &QAction::triggered,
                     this, &MainWindow::sendCurrentCatalogueViaSamp);
}


void MainWindow::buildScienceMenu()
{
    auto *sciMenu = new QMenu(u"Science"_s, this);
    this->menuBar()->insertMenu(ui->menuHelp->menuAction(), sciMenu);
    m_scienceMenu = sciMenu;

    m_actLinewidth = sciMenu->addAction(u"Line-Width Map…"_s);
    m_actLinewidth->setEnabled(false);
    m_actLinewidth->setToolTip(u"Compute FWHM / Equivalent-Width map (S-02). Open a dataset first."_s);
    QObject::connect(m_actLinewidth, &QAction::triggered, this, [this]() {
        auto *dlg = new LinewidthDialog(m_sciBackendUrl, m_sciToken,
                                        m_sciSessionId, m_sciDatasetId,
                                        m_sciDepth, m_sciThreshold,
                                        m_sciSpectralAxisType, m_sciSpectralAxisUnit,
                                        this);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->show();
    });

    m_actBaseline = sciMenu->addAction(u"Baseline Subtraction…"_s);
    m_actBaseline->setEnabled(false);
    m_actBaseline->setToolTip(u"Subtract a polynomial or median spectral baseline (S-03). Open a dataset first."_s);
    QObject::connect(m_actBaseline, &QAction::triggered, this, [this]() {
        auto *dlg = new BaselineDialog(m_sciBackendUrl, m_sciToken,
                                       m_sciSessionId, m_sciDatasetId,
                                       m_sciDepth, this);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->show();
    });

    m_actStack = sciMenu->addAction(u"Spectral Stacking…"_s);
    m_actStack->setEnabled(true);  // no dataset needed upfront — user selects from list
    m_actStack->setToolTip(u"Stack multiple session datasets into a 1-D spectrum (S-04)."_s);
    QObject::connect(m_actStack, &QAction::triggered, this, [this]() {
        const QString url       = m_sciBackendUrl;
        const QString token     = m_sciToken;
        const QString sessionId = m_sciSessionId;
        const QString currentId = m_sciDatasetId;
        if (sessionId.isEmpty()) {
            QMessageBox::information(this, u"Spectral Stacking"_s,
                                     u"Open a dataset first to establish a backend session, "
                                     u"then open the additional cubes you want to stack."_s);
            return;
        }
        auto *watcher = new QFutureWatcher<BackendSessionDatasetsResult>(this);
        QPointer<MainWindow> self(this);
        QObject::connect(watcher, &QFutureWatcher<BackendSessionDatasetsResult>::finished,
                         this, [self, watcher, url, token, sessionId, currentId]() {
                             const auto listing = watcher->result();
                             watcher->deleteLater();
                             if (!self) return;
                             if (!listing.valid) {
                                 QMessageBox::warning(self, u"Spectral Stacking"_s,
                                                      u"Could not list session datasets: %1"_s
                                                              .arg(listing.error));
                                 return;
                             }
                             std::vector<StackDialog::DatasetEntry> entries;
                             entries.reserve(listing.datasets.size());
                             for (const auto &d : listing.datasets) {
                                 const QString fileName = QFileInfo(d.path).fileName();
                                 const QString label = fileName.isEmpty() ? d.datasetId : fileName;
                                 entries.push_back({ d.datasetId, d.kind, label,
                                                     d.width, d.height, d.depth });
                             }
                             // Shared ownership: the async stack run captures a
                             // copy of the shared_ptr, so the client survives an
                             // early dialog close (no use-after-free).
                             auto client = std::make_shared<BackendClient>(url, token);
                             client->setSessionId(sessionId);
                             auto *dlg = new StackDialog(client, sessionId, entries,
                                                         currentId, self);
                             dlg->setAttribute(Qt::WA_DeleteOnClose);
                             // Non-modal — consistent with the other tool dialogs.
                             dlg->show();
                         });
        watcher->setFuture(QtConcurrent::run([url, token, sessionId]() {
            BackendClient c(url, token);
            c.setSessionId(sessionId);
            return c.listSessionDatasets(sessionId);
        }));
    });

    m_actSourceFind = sciMenu->addAction(u"Source Finding (SoFiA-2)…"_s);
    m_actSourceFind->setEnabled(false);
    m_actSourceFind->setToolTip(u"Run SoFiA-2 on the current dataset (S-05). Open a dataset first."_s);
    QObject::connect(m_actSourceFind, &QAction::triggered, this, [this]() {
        this->openSourceFinder(m_sciBackendUrl, m_sciToken, m_sciSessionId, m_sciDatasetId);
    });

    sciMenu->addSeparator();

    // ── Advanced 2-D products (SKAVA visualization use cases) ────────────
    m_actPubFigure = sciMenu->addAction(u"Publication Figure…"_s);
    m_actPubFigure->setEnabled(false);
    m_actPubFigure->setToolTip(u"Render a publication-quality figure with WCS grid + colorbar (UC 1.3). Open a dataset first."_s);
    QObject::connect(m_actPubFigure, &QAction::triggered, this, &MainWindow::runPublicationFigure);

    m_actImageQuality = sciMenu->addAction(u"Image Quality / Artifacts…"_s);
    m_actImageQuality->setEnabled(false);
    m_actImageQuality->setToolTip(u"Compute quality/artifact metrics: blanked %, dynamic range, striping, negative bowl (UC 4.2)."_s);
    QObject::connect(m_actImageQuality, &QAction::triggered, this, &MainWindow::runImageQuality);

    m_actPixelMath = sciMenu->addAction(u"Pixel Math / Spectral Index…"_s);
    m_actPixelMath->setEnabled(true);  // selects datasets from the session list
    m_actPixelMath->setToolTip(u"Pixel-wise math on one/two maps, incl. spectral index (UC 3.2)."_s);
    QObject::connect(m_actPixelMath, &QAction::triggered, this, &MainWindow::runPixelMath);

    m_actMosaic = sciMenu->addAction(u"Mosaic (noise-weighted)…"_s);
    m_actMosaic->setEnabled(true);
    m_actMosaic->setToolTip(u"Reproject + co-add multiple images into a noise-weighted mosaic (UC 3.3)."_s);
    QObject::connect(m_actMosaic, &QAction::triggered, this, &MainWindow::runMosaic);
}
