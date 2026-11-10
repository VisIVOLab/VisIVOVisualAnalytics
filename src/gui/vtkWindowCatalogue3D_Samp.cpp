// vtkWindowCatalogue3D_Samp.cpp – SAMP row linking (issue #130)
//
// table.highlight.row and table.select.rowList name rows by their 0-based
// position in the ORIGINAL table. This window may hold a filtered, sorted or
// evenly sampled subset of it ("100000 of 3931546 rows"), so every row is
// looked up through the row each entry came from (Catalogue3DEntry::sourceRow,
// which the backend's query reports), never used as an entry index. A row
// that is not loaded here is reported, not guessed.

#include "vtkWindowCatalogue3D.h"
#include "ui_vtkWindowCatalogue3D.h"

#include "CatalogueFormat.h"
#include "MainWindow.h"
#include "SampViewerMenu.h"
#include "app/SampSkyPosition.h"

#include <QAction>
#include <QItemSelectionModel>
#include <QLabel>
#include <QMenu>
#include <QPointer>
#include <QStatusBar>
#include <QTableView>
#include <QStackedWidget>
#include <QScopeGuard>

#include <vtkActor.h>
#include <vtkFloatArray.h>
#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkGlyph3DMapper.h>
#include <vtkPointData.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>

using namespace Qt::StringLiterals;

bool vtkWindowCatalogue3D::sampShowsLocalView() const
{
    return !this->m_renderStack || this->m_renderStack->currentWidget() == ui->vtk;
}

QString vtkWindowCatalogue3D::sampLocalViewNote() const
{
    if (this->sampShowsLocalView())
        return {};
    // The selection and the marks are drawn by the local view only. The
    // ray-traced picture replaces it through its own check, not the Rendering
    // menu, so that is the control named there.
    if (this->geometryMode == GeometryMode::Raytraced)
        return u" Marks and recentring show once the view is not ray-traced "
               u"(Source Representation ▸ Ray-traced light off)."_s;
    return u" Marks and recentring show in the local view (View ▸ Rendering ▸ Local)."_s;
}

void vtkWindowCatalogue3D::showSelectionStatus(const QString &text)
{
    this->m_selectionStatus = text;
    ui->labelStatus->setText(text);
}

QString vtkWindowCatalogue3D::sampTableKey() const
{
    return this->m_cataloguePaths.size() > 1 ? QString() : this->filepath;
}

void vtkWindowCatalogue3D::ensureSampRowMap()
{
    if (!this->m_sampRowMapDirty && this->m_sampRowMapSize == this->entries.size())
        return;
    std::vector<qint64> rows;
    rows.reserve(this->entries.size());
    for (const Catalogue3DEntry &e : this->entries)
        rows.push_back(e.sourceRow);
    this->m_sampRowMap.reset(rows);
    this->m_sampRowMapSize = this->entries.size();
    this->m_sampRowMapDirty = false;
}

bool vtkWindowCatalogue3D::highlightSampRow(qint64 row, const QString &from)
{
    this->ensureSampRowMap();
    const int idx = this->m_sampRowMap.entryFor(row);
    if (idx < 0 || static_cast<std::size_t>(idx) >= this->mappedPositions.size()) {
        QString why;
        if (this->m_sampRowMap.isEmpty() && !this->entries.empty())
            why = u"the backend does not report row numbers; update it to link rows"_s;
        else if (this->hasMoreRows())
            why = u"it is not loaded yet, or is filtered out: Load more or Load all may bring it"_s;
        else if (!this->activeFilters.empty())
            why = u"it is filtered out"_s;
        else
            why = u"the table has no such row"_s;
        // Rows are counted from 1 for people (TOPCAT's own row numbers), from
        // 0 on the wire.
        this->showSelectionStatus(u"%1 highlighted row %2, which is not shown here: %3."_s
                                          .arg(from)
                                          .arg(row + 1)
                                          .arg(why));
        return false;
    }

    // Select it as a click would (Source Info, the table, the white sphere),
    // without sending it back to the tools it came from.
    this->m_applyingSampSelection = true;
    const auto restore = qScopeGuard([this]() { this->m_applyingSampSelection = false; });
    this->setSelectedSource(idx);
    // Recentre at the same distance: a zoom on every highlight would dive
    // further in with each row the other tool steps through.
    this->centerOnSource(idx, 1.0);
    this->showSelectionStatus(u"%1 highlighted row %2."_s.arg(from).arg(row + 1)
                              + this->sampLocalViewNote());
    return true;
}

void vtkWindowCatalogue3D::selectSampRows(const QList<qint64> &rows, const QString &from)
{
    this->m_sampSubsetRows = rows;
    this->m_sampSubsetFrom = from;
    this->updateSampSubset(true);
    this->renderWindow->Render();
}

void vtkWindowCatalogue3D::updateSampSubset(bool report)
{
    this->ensureSampRowMap();
    const SampRowMap::Split split = this->m_sampRowMap.entriesFor(this->m_sampSubsetRows);

    vtkNew<vtkPoints> points;
    this->m_sampSubsetScales->SetName("SampMarkRadius");
    this->m_sampSubsetScales->SetNumberOfComponents(1);
    this->m_sampSubsetScales->Reset();
    points->Allocate(split.entries.size());
    for (const int idx : split.entries) {
        if (static_cast<std::size_t>(idx) >= this->mappedPositions.size())
            continue;
        const auto &p = this->mappedPositions[static_cast<std::size_t>(idx)];
        points->InsertNextPoint(p[0], p[1], p[2]);
        this->m_sampSubsetScales->InsertNextValue(
                static_cast<float>(this->highlightRadius(idx, 1.4)));
    }
    this->m_sampSubsetPoints->DeepCopy(points);
    this->m_sampSubsetData->SetPoints(this->m_sampSubsetPoints);
    this->m_sampSubsetData->GetPointData()->AddArray(this->m_sampSubsetScales);
    this->m_sampSubsetData->Modified();
    this->m_sampSubsetActor->SetVisibility(points->GetNumberOfPoints() > 0);

    if (!report)
        return;
    const QString note = this->sampLocalViewNote();
    const QString from = this->m_sampSubsetFrom.isEmpty() ? u"A SAMP tool"_s
                                                          : this->m_sampSubsetFrom;
    if (this->m_sampSubsetRows.isEmpty()) {
        ui->labelStatus->setText(u"%1 selected no rows: marks cleared."_s.arg(from));
        return;
    } else if (split.missing == 0) {
        ui->labelStatus->setText(u"%1 selected %2 rows, all marked here."_s
                                         .arg(from)
                                         .arg(this->m_sampSubsetRows.size())
                                 + note);
    } else {
        ui->labelStatus->setText(
                u"%1 selected %2 rows: %3 marked here, %4 not shown "
                u"(filtered out, or not loaded yet)."_s
                        .arg(from)
                        .arg(this->m_sampSubsetRows.size())
                        .arg(split.entries.size())
                        .arg(split.missing)
                + note);
    }
}

// ── SAMP actions (issue #130) ────────────────────────────────────────────────
//
// What this window sends: the catalogue (the backend's file), a selection of
// its rows as a new VOTable (rows picked in the table, or every row loaded
// here when a filter, a sample or a partial load shows fewer than the table
// has), the selection as table.select.rowList to the tools that already hold
// the table, and the selected source's RA/Dec as coord.pointAt.sky.

void vtkWindowCatalogue3D::setupSampMenu()
{
    visivo::installSampMenu(this, [this](QMenu *menu, MainWindow *host) {
        this->fillSampMenu(menu, host, /*selectionOnly=*/false);
    });
    if (!this->catalogueTableView)
        return;
    // The rows picked in the table are what its context menu acts on.
    this->catalogueTableView->setContextMenuPolicy(Qt::CustomContextMenu);
    QObject::connect(this->catalogueTableView, &QWidget::customContextMenuRequested, this,
                     [this](const QPoint &pos) {
                         QMenu menu(this->catalogueTableView);
                         menu.setToolTipsVisible(true);
                         MainWindow *host = visivo::sampHost(this);
                         visivo::addSampConnectionLine(&menu, host);
                         this->fillSampMenu(&menu, host, /*selectionOnly=*/true);
                         menu.exec(this->catalogueTableView->viewport()->mapToGlobal(pos));
                     });
}

QList<qint64> vtkWindowCatalogue3D::sampSelectedRows()
{
    this->ensureSampRowMap();
    QList<int> picked;
    if (this->catalogueTableView && this->catalogueTableView->selectionModel()) {
        for (const QModelIndex &index : this->catalogueTableView->selectionModel()->selectedRows())
            picked.append(index.row());
    }
    if (picked.isEmpty() && this->selectedIndex >= 0)
        picked.append(this->selectedIndex);
    return this->m_sampRowMap.rowsForEntries(picked);
}

qsizetype vtkWindowCatalogue3D::sampSelectedCount() const
{
    // From the selection's ranges, not its rows: a menu opened over a
    // selection of a million rows must not list them all to count them.
    qsizetype count = 0;
    if (this->catalogueTableView && this->catalogueTableView->selectionModel()) {
        for (const QItemSelectionRange &range : this->catalogueTableView->selectionModel()->selection())
            count += range.height();
    }
    return count > 0 ? count : (this->selectedIndex >= 0 ? 1 : 0);
}

QList<qint64> vtkWindowCatalogue3D::sampShownRows()
{
    this->ensureSampRowMap();
    QList<int> all;
    all.reserve(static_cast<qsizetype>(this->entries.size()));
    for (std::size_t i = 0; i < this->entries.size(); ++i)
        all.append(static_cast<int>(i));
    return this->m_sampRowMap.rowsForEntries(all);
}

bool vtkWindowCatalogue3D::sampShowsFewerRows() const
{
    return !this->activeFilters.empty() || this->hasMoreRows()
           || static_cast<qint64>(this->entries.size()) < this->tableRowCount();
}

void vtkWindowCatalogue3D::sendSelectedSourcePosition(const QString &recipientId, bool quiet)
{
    MainWindow *host = visivo::sampHost(this);
    const int idx = this->selectedIndex;
    QString problem;
    if (!this->m_skyCoordinates)
        problem = u"this catalogue has no RA/Dec columns"_s;
    else if (idx < 0 || static_cast<std::size_t>(idx) >= this->entries.size())
        problem = u"select a source first"_s;
    else if (!host || !host->sampConnected())
        problem = u"not connected to a SAMP hub"_s;
    if (!problem.isEmpty()) {
        if (!quiet)
            this->statusBar()->showMessage(u"SAMP: no position sent: %1."_s.arg(problem), 6000);
        return;
    }
    const Catalogue3DEntry &e = this->entries[static_cast<std::size_t>(idx)];
    // The catalogue's RA/Dec, as read: J2000 (FK5 and ICRS agree to 25 mas).
    if (!host->sendSkyPositionViaSamp(e.raDeg, e.decDeg, recipientId)) {
        if (!quiet)
            this->statusBar()->showMessage(u"SAMP: the source has no valid position."_s, 6000);
        return;
    }
    const QString name = Catalogue3DParser::detail::entryDisplayName(e, this->schema);
    this->statusBar()->showMessage(
            u"SAMP: sent the position of %1 (%2)."_s.arg(
                    name, visivo::samp::describe({ e.raDeg, e.decDeg })),
            6000);
}

void vtkWindowCatalogue3D::fillSampMenu(QMenu *menu, MainWindow *host, bool selectionOnly)
{
    const QString key = this->sampTableKey();
    const QString name = this->shownName();
    const bool combined = key.isEmpty();
    const bool busy = host && host->sampSendBusy();
    const QString busyWhy = u"A file is still being prepared for sending."_s;
    const QString combinedWhy =
            u"A catalogue combined from several files has no one table to send."_s;
    const QString format = visivo::catalogueFormatForPath(this->filepath);
    const QString wholeMType = Samp::catalogueLoadMType(format);
    const QString votable = QString::fromLatin1(Samp::kTableLoadVotable);
    QPointer<vtkWindowCatalogue3D> self = this;

    if (!selectionOnly) {
        visivo::addSampSendTo(menu, host, u"Send Catalogue To"_s, wholeMType,
                              combined ? combinedWhy : (busy ? busyWhy : QString()),
                              [self, name](const QString &to) {
                                  MainWindow *mw = self ? visivo::sampHost(self) : nullptr;
                                  if (mw)
                                      mw->sendCatalogueFileViaSamp(self->filepath, name, to, self);
                              });
    }

    // Rows picked in the table (or the selected source). Counted only: the
    // rows themselves are listed when an item is chosen.
    const qsizetype selectedCount = this->sampSelectedCount();
    this->ensureSampRowMap();
    const bool noRowNumbers = this->m_sampRowMap.isEmpty() && !this->entries.empty();
    const QString noRowsWhy =
            u"The backend does not report row numbers; update it to send a selection."_s;
    QString selectedWhy;
    if (combined)
        selectedWhy = combinedWhy;
    else if (noRowNumbers)
        selectedWhy = noRowsWhy;
    else if (selectedCount == 0)
        selectedWhy = u"Select rows in the table first (Shift or Cmd/Ctrl for several)."_s;
    else if (busy)
        selectedWhy = busyWhy;
    const QString selectedTitle = selectedCount == 1
            ? u"Send Selected Row To"_s
            : u"Send %L1 Selected Rows To"_s.arg(selectedCount);
    // Sends a list of rows, or says there is none (rows of no known number).
    const auto sendRows = [self, name](const QList<qint64> &rows, const QString &to) {
        MainWindow *mw = self ? visivo::sampHost(self) : nullptr;
        if (!mw)
            return;
        if (rows.isEmpty()) {
            self->statusBar()->showMessage(u"SAMP: none of those rows has a row number."_s, 6000);
            return;
        }
        mw->sendCatalogueRowsViaSamp(self->filepath, name, rows, to, self);
    };
    visivo::addSampSendTo(menu, host, selectedCount == 0 ? u"Send Selected Rows To"_s : selectedTitle,
                          votable, selectedWhy, [self, sendRows](const QString &to) {
                              if (self)
                                  sendRows(self->sampSelectedRows(), to);
                          });

    if (!selectionOnly && this->sampShowsFewerRows()) {
        const qsizetype shownCount = static_cast<qsizetype>(this->entries.size());
        visivo::addSampSendTo(menu, host, u"Send the %L1 Rows Shown To"_s.arg(shownCount), votable,
                              combined         ? combinedWhy
                              : noRowNumbers   ? noRowsWhy
                              : shownCount == 0 ? u"No rows are shown."_s
                              : busy            ? busyWhy
                                                : QString(),
                              [self, sendRows](const QString &to) {
                                  if (self)
                                      sendRows(self->sampShownRows(), to);
                              });
    }

    // The same rows, selected in the table the other tools already hold.
    QAction *select = menu->addAction(selectedCount > 1
                                              ? u"Select These %L1 Rows in Other Tools"_s.arg(
                                                        selectedCount)
                                              : u"Select These Rows in Other Tools"_s);
    const bool held = host && host->sampToolsHoldTable(key);
    select->setEnabled(host && host->sampConnected() && held && selectedCount > 0
                       && !noRowNumbers);
    select->setToolTip(
            held ? u"table.select.rowList: the tools that hold this table (it came from them, "
                   u"or VisIVO sent it) select the same rows."_s
                 : u"No other tool holds this table yet: send it first (Send Catalogue To), "
                   u"or open it from TOPCAT."_s);
    QObject::connect(select, &QAction::triggered, this, [self, key]() {
        MainWindow *mw = self ? visivo::sampHost(self) : nullptr;
        if (!mw)
            return;
        const QList<qint64> selected = self->sampSelectedRows();
        const int tables = mw->selectRowsInSampTools(key, selected);
        self->statusBar()->showMessage(
                tables > 0 ? u"SAMP: selected %L1 rows in the other tools."_s.arg(selected.size())
                           : u"SAMP: no other tool holds this table."_s,
                6000);
    });

    menu->addSeparator();
    QString positionWhy;
    if (!this->m_skyCoordinates)
        positionWhy = u"This catalogue has no RA/Dec columns."_s;
    else if (this->selectedIndex < 0)
        positionWhy = u"Select a source first."_s;
    visivo::addSampSendTo(menu, host, u"Send Source Position To"_s,
                          QString::fromLatin1(Samp::kCoordPointAtSky), positionWhy,
                          [self](const QString &to) {
                              if (self)
                                  self->sendSelectedSourcePosition(to, /*quiet=*/false);
                          });
    if (selectionOnly)
        return;
    QAction *broadcast = menu->addAction(u"Send Positions of Clicked Sources"_s);
    broadcast->setCheckable(true);
    broadcast->setChecked(this->m_sampBroadcastClicks);
    broadcast->setEnabled(this->m_skyCoordinates);
    broadcast->setToolTip(
            u"While on, every source you select is sent to all SAMP tools as "
            u"coord.pointAt.sky (Aladin and DS9 recentre on it)."_s);
    QObject::connect(broadcast, &QAction::toggled, this, [self](bool on) {
        if (self)
            self->m_sampBroadcastClicks = on;
    });
}
