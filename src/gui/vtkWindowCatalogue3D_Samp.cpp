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

#include <QLabel>
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
    // ray-traced picture replaces it through the Shape, not the Rendering
    // menu, so that is the control named there.
    if (this->geometryMode == GeometryMode::Raytraced)
        return u" Marks and recentring show with the other shapes "
               u"(Source Representation ▸ Shape, not Ray-traced)."_s;
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
