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

        // ── Combine ─────────────────────────────────────────────────────────
        // Only the multi-dataset operations. The palette lives on the Data Hub,
        // and the per-view tools it used to list (Line-Width, Baseline, Stack,
        // Source Finding) moved into the viewers: triggering them from here
        // would be triggering them against no particular window, which is the
        // problem that move was fixing. They are in each viewer's own Analysis
        // list and Tools menu.
        this->m_commandPalette->addAction(
                {u"÷"_s, u"Pixel Math / Spectral Index…"_s,
                 u"Combine · pixel-wise math on one or two session maps"_s,
                 QString(), u"Combine"_s, [this]() {
                     if (this->m_actPixelMath) this->m_actPixelMath->trigger();
                 }});
        this->m_commandPalette->addAction(
                {u"▦"_s, u"Mosaic (noise-weighted)…"_s,
                 u"Combine · reproject and co-add session images"_s,
                 QString(), u"Combine"_s, [this]() {
                     if (this->m_actMosaic) this->m_actMosaic->trigger();
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
                                subtitle = a->isChecked() ? u"On, select to turn off"_s
                                                          : u"Off, select to turn on"_s;
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


void MainWindow::buildCombineMenu()
{
    // The Data Hub keeps only what has NO owning window: operations that take
    // SEVERAL of the session's datasets and produce a new one. Everything else
    // that used to live here was a tool of a single view, resolved against
    // "the last dataset opened" — a sticky global with no relation to the window
    // the user was looking at, so with two cubes open it acted on the wrong
    // one, and before any dataset was opened it could only answer "Open a
    // dataset first" (user report). Those moved to the viewers, where the tool
    // gate already knows what is on screen:
    //
    //   Line-Width Map, Baseline Subtraction, Stack Cubes to a Spectrum
    //       → already in the cube viewer's Analysis list (this was a duplicate)
    //   Source Finding (SoFiA-2)      → cube viewer, Sources & Kinematics
    //   Publication Figure,
    //   Image Quality / Artifacts     → image viewer, Products
    //
    // "Science" was also the wrong name for what is left: everything in this
    // application is science. What these two share is scope.
    auto *combineMenu = new QMenu(u"Combine"_s, this);
    this->menuBar()->insertMenu(ui->menuHelp->menuAction(), combineMenu);
    m_scienceMenu = combineMenu;

    m_actPixelMath = combineMenu->addAction(u"Pixel Math / Spectral Index…"_s);
    QObject::connect(m_actPixelMath, &QAction::triggered, this, &MainWindow::runPixelMath);

    m_actMosaic = combineMenu->addAction(u"Mosaic (noise-weighted)…"_s);
    QObject::connect(m_actMosaic, &QAction::triggered, this, &MainWindow::runMosaic);

    // Both need a session with datasets in it, not a "current" one. Enabled
    // state AND tooltips come from refreshCombineMenuState, so the menu says
    // what it can do instead of offering an action that explains it cannot.
    this->refreshCombineMenuState();
}

void MainWindow::refreshCombineMenuState()
{
    // A viewer window IS the evidence that a session with a dataset exists, and
    // any of them can supply the session context. Two datasets are needed for
    // either operation to be meaningful, so that is the bar.
    int viewers = 0;
    for (QWidget *w : QApplication::topLevelWidgets()) {
        if (w && (w->inherits("vtkWindowCube") || w->inherits("vtkWindowImage")))
            ++viewers;
    }
    const bool enough = viewers >= 1 || !m_sciSessionId.isEmpty();
    // Each entry's own description when it can run, the reason when it cannot.
    // Overwriting the description on the way down and never restoring it left
    // an enabled action still explaining that it needed a dataset.
    static constexpr QLatin1StringView kBlocked(
            "Open a dataset first: these combine datasets that are already open "
            "in this session.");
    const auto apply = [enough](QAction *a, const QString &ready) {
        if (!a)
            return;
        a->setEnabled(enough);
        a->setToolTip(enough ? ready : QString(kBlocked));
    };
    apply(m_actPixelMath,
          u"Pixel-wise math on one or two maps from this session, including the "
          u"spectral index between two frequencies. Pick the inputs from the "
          u"session list."_s);
    apply(m_actMosaic,
          u"Reproject and co-add several images from this session onto one grid, "
          u"weighted by their noise. Produces a 2-D mosaic: cube inputs "
          u"contribute their first plane only."_s);
}
