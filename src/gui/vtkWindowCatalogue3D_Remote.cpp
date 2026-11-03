// vtkWindowCatalogue3D_Remote.cpp
// A VBT point table drawn on the backend (issue #83): the render mode
// (Auto / Local / Remote), the streamed view beside the local one, and the
// view's state sent to the backend's point session (app/render_session/points.py).

#include "vtkWindowCatalogue3D.h"
#include "ui_vtkWindowCatalogue3D.h"

#include "BackendLocality.h"
#include "RemotePointView.h"
#include "RemoteRenderWidget.h"
#include "LUTCustomizerDialog.h"
#include "VolumeColorbar.h"
#include "Catalogue3DParser.h"
#include "ColorMaps.h"
#include "LutScale.h"

#include <QActionGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QMenu>
#include <QSlider>
#include <QStackedWidget>
#include <QStatusBar>
#include <QVBoxLayout>
#include <QFormLayout>
#include <QTableView>

#include <algorithm>
#include <cmath>

#include <vtkColorTransferFunction.h>
#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkFloatArray.h>
#include <vtkImageData.h>
#include <vtkPointData.h>
#include <vtkLookupTable.h>
#include <vtkNew.h>

using namespace Qt::Literals::StringLiterals;

void vtkWindowCatalogue3D::setupRemoteRendering()
{
    // The local view and the streamed one, in the place the local one had.
    auto *layout = ui->vtk->parentWidget() ? ui->vtk->parentWidget()->layout() : nullptr;
    if (!layout)
        return;
    this->m_renderStack = new QStackedWidget(ui->vtk->parentWidget());
    layout->replaceWidget(ui->vtk, this->m_renderStack);
    this->m_renderStack->addWidget(ui->vtk);
    this->m_remoteWidget = new RemoteRenderWidget(this->m_renderStack);
    this->m_renderStack->addWidget(this->m_remoteWidget);

    // The colour bar of the cube viewer's 3-D view, over whichever view shows.
    this->m_colorbar = new VolumeColorbar(this);
    this->m_colorbar->hide();
    this->m_colorbar->attachTo(ui->vtk);

    // View ▸ Rendering, as in the cube viewer.
    auto *menu = new QMenu(u"Rendering"_s, this);
    auto *group = new QActionGroup(this);
    group->setExclusive(true);
    const auto add = [&](const QString &text, const QString &tip, RenderMode mode) {
        QAction *a = menu->addAction(text);
        a->setCheckable(true);
        a->setToolTip(tip);
        group->addAction(a);
        QObject::connect(a, &QAction::triggered, this,
                         [this, mode]() { this->applyRenderMode(mode); });
        return a;
    };
    add(u"Auto"_s, u"On the backend when the table has more rows than this window loaded"_s,
        RenderMode::Auto)
            ->setChecked(true);
    add(u"Local (CPU/GPU on this machine)"_s, u"The rows loaded in this window, drawn here"_s,
        RenderMode::Local);
    this->m_actRenderRemote = add(u"Remote (GPU on backend)"_s,
                                  u"Every row, drawn on the backend and streamed"_s,
                                  RenderMode::Remote);
    menu->setToolTipsVisible(true);
    ui->menuView->insertMenu(ui->menuView->actions().value(2), menu);

    this->m_renderBadge = new QLabel(this);
    this->statusBar()->addPermanentWidget(this->m_renderBadge);
    this->m_renderBadge->hide();
}

int vtkWindowCatalogue3D::rowsToShow() const
{
    // The rows matching the filters once a query has counted them, else the
    // whole table.
    return this->m_totalCount >= 0 ? this->m_totalCount : this->m_tableRows;
}

bool vtkWindowCatalogue3D::wantsRemote() const
{
    // Never with the local backend: it draws here; never mixed.
    if (!this->m_serverSideRendering || visivo::backendIsLocal(this->backendUrl))
        return false;
    if (this->m_renderMode == RenderMode::Remote)
        return true;
    // Auto: the table (with the filters applied) holds more rows than this
    // window loaded.
    return this->m_renderMode == RenderMode::Auto
            && this->rowsToShow() > static_cast<int>(this->entries.size());
}

void vtkWindowCatalogue3D::applyRenderMode(RenderMode mode)
{
    this->m_renderMode = mode;
    if (this->m_actRenderRemote) {
        const bool canRemote =
                this->m_serverSideRendering && !visivo::backendIsLocal(this->backendUrl);
        this->m_actRenderRemote->setEnabled(canRemote);
        if (!canRemote)
            this->m_actRenderRemote->setToolTip(
                    !this->m_serverSideRendering
                            ? u"Only a VisIVO Binary Table of points, on a backend that "
                              u"renders server-side (EGL)"_s
                            : u"The local backend: the points are drawn on this machine"_s);
    }
    if (!this->wantsRemote()) {
        if (RemotePointView *view = this->m_remoteView) {
            this->m_remoteView = nullptr;
            view->stop();
            view->deleteLater();
        }
        if (this->m_renderStack)
            this->m_renderStack->setCurrentIndex(0);
        if (this->m_colorbar)
            this->m_colorbar->attachTo(ui->vtk);
        // Back to the rows loaded here: their own colour range, and the
        // automatic Log/Linear choice made on it.
        if (this->m_colorRemoteRange[1] > this->m_colorRemoteRange[0]) {
            this->m_colorRemoteRange[0] = this->m_colorRemoteRange[1] = 0.0;
            if (!(this->m_colorUserRange[1] > this->m_colorUserRange[0])) {
                if (this->comboColorScale) {
                    const QSignalBlocker block(this->comboColorScale);
                    this->comboColorScale->setCurrentIndex(
                            static_cast<int>(LutScale::Mode::Linear));
                }
                this->m_defaultScaleChosen = false;
            }
            this->updateColorMapping();
            this->renderWindow->Render();
        }
        this->updateRenderBadge();
        return;
    }
    if (!this->m_remoteView) {
        this->m_remoteView = new RemotePointView(this->backendUrl, this->backendToken,
                                                 this->remoteSessionId, this->remoteDatasetId,
                                                 this->m_remoteWidget, this);
        QObject::connect(this->m_remoteView, &RemotePointView::failed, this,
                         [this](const QString &reason) {
                             this->statusBar()->showMessage(
                                     u"Not drawn on the backend: %1. Showing the loaded rows."_s
                                             .arg(reason),
                                     8000);
                             if (RemotePointView *view = this->m_remoteView) {
                                 this->m_remoteView = nullptr;
                                 view->deleteLater();
                             }
                             // The rows loaded here, coloured over their range.
                             this->m_colorRemoteRange[0] = this->m_colorRemoteRange[1] = 0.0;
                             this->updateColorMapping();
                             if (this->m_renderStack)
                                 this->m_renderStack->setCurrentIndex(0);
                             if (this->m_colorbar)
                                 this->m_colorbar->attachTo(ui->vtk);
                             this->updateRenderBadge();
                         });
        // The colour column's range over every row the backend draws: the one
        // the colours and the bar use, unless Min..Max were set in Edit.
        QObject::connect(this->m_remoteView, &RemotePointView::colourRange, this,
                         [this](double lo, double hi) {
                             if (!(hi > lo) || (lo == this->m_colorRemoteRange[0]
                                                && hi == this->m_colorRemoteRange[1]))
                                 return;
                             this->m_colorRemoteRange[0] = lo;
                             this->m_colorRemoteRange[1] = hi;
                             if (this->m_colorUserRange[1] > this->m_colorUserRange[0])
                                 return;
                             // The automatic Log/Linear choice, on this range.
                             if (this->comboColorScale) {
                                 const QSignalBlocker block(this->comboColorScale);
                                 this->comboColorScale->setCurrentIndex(
                                         static_cast<int>(LutScale::Mode::Linear));
                             }
                             this->m_defaultScaleChosen = false;
                             this->updateColorMapping();
                         });
        QObject::connect(this->m_remoteView, &RemotePointView::started, this, [this]() {
            if (this->m_renderStack)
                this->m_renderStack->setCurrentIndex(1);
            if (this->m_colorbar)
                this->m_colorbar->attachTo(this->m_remoteWidget);
            this->updateRenderBadge();
        });
        this->pushRemoteState();
        this->m_remoteView->start();
    }
    this->updateRenderBadge();
}

void vtkWindowCatalogue3D::updateRenderBadge()
{
    if (!this->m_renderBadge)
        return;
    if (!this->m_serverSideRendering) {
        this->m_renderBadge->hide();
        return;
    }
    const bool remote = this->m_remoteView && this->m_remoteView->isActive();
    this->m_renderBadge->setText(
            remote ? u"Remote · all %L1 rows"_s.arg(this->rowsToShow())
                   : u"Local · %L1 of %L2 rows"_s.arg(this->entries.size()).arg(this->rowsToShow()));
    this->m_renderBadge->show();
}

void vtkWindowCatalogue3D::pushRemoteState()
{
    if (!this->m_remoteView)
        return;
    RemotePointView *view = this->m_remoteView;

    // Coordinates: a column ("raw:<index>" into the table's columns), or the
    // table's own X/Y/Z ("computed:"), which for a cartesian table are its
    // X/Y/Z columns.
    const auto column = [this](const QString &key, const QString &computed) {
        return key.startsWith(u"raw:"_s) ? this->rawHeaders.value(key.mid(4).toInt()) : computed;
    };
    view->setState({ { u"type"_s, u"set_point_fields"_s },
                     { u"x"_s, column(this->mapXKey, this->schema.xFieldName) },
                     { u"y"_s, column(this->mapYKey, this->schema.yFieldName) },
                     { u"z"_s, column(this->mapZKey, this->schema.zFieldName) } });

    // Filters: the backend's view filters on numbers (a VBT holds nothing else).
    static const QStringList numericOps{ u"="_s, u"!="_s, u">"_s, u">="_s, u"<"_s, u"<="_s };
    QVariantList filters;
    bool skipped = false;
    for (const BackendTabularFilter &f : this->activeFilters) {
        bool number = false;
        f.value.trimmed().toDouble(&number);
        if (!numericOps.contains(f.op) || !number) {
            skipped = true;
            continue;
        }
        filters << QVariantMap{ { u"field"_s, f.field }, { u"op"_s, f.op },
                                { u"value"_s, f.value } };
    }
    if (skipped)
        this->statusBar()->showMessage(
                u"Text filters (contains, in, or a value that is not a number) apply to "
                u"the loaded rows, not to the backend's view."_s,
                6000);
    view->setState({ { u"type"_s, u"set_filters"_s }, { u"filters"_s, filters } });

    // Colours: the column and the very colours drawn here, point by point (the
    // ramp, its scale and its range), so both views read the same.
    QVariantMap color{ { u"type"_s, u"set_color"_s } };
    const bool coloured =
            !ui->chkMorphColors || this->m_noMorphology || ui->chkMorphColors->isChecked();
    if (coloured && this->mapColorKey.startsWith(u"raw:"_s)) {
        vtkNew<vtkColorTransferFunction> ctf;
        ColorMaps::SetColorTransferFunction(this->valueLut, ctf);
        QVariantList points;
        for (int i = 0; i < ctf->GetSize(); ++i) {
            double node[6];
            ctf->GetNodeValue(i, node);
            points << QVariant(QVariantList{ node[0], node[1], node[2], node[3] });
        }
        const double *range = this->valueLut->GetTableRange();
        color.insert(u"field"_s, column(this->mapColorKey, QString()));
        color.insert(u"points"_s, points);
        color.insert(u"log"_s, this->valueLut->GetScale() == VTK_SCALE_LOG10);
        color.insert(u"min"_s, range[0]);
        color.insert(u"max"_s, range[1]);
    } else {
        // Morphology and derived quantities exist only here: solid colour there.
        color.insert(u"field"_s, QVariant());
    }
    view->setState(color);

    const bool gaussian = this->geometryMode == GeometryMode::Gaussian;
    view->setState({ { u"type"_s, u"set_point_style"_s },
                     { u"mode"_s, gaussian ? u"gaussian"_s : u"points"_s },
                     // Points in pixels: the backend draws in physical ones.
                     // Splats: the same continuous size step as here.
                     { u"size"_s, gaussian ? QVariant(this->splatSizeFromScale())
                                           : QVariant(static_cast<int>(std::lround(
                                                     this->pointSizeFromScale()
                                                     * this->devicePixelRatioF()))) },
                     { u"intensity"_s, 35 } });
    view->setState({ { u"type"_s, u"set_show_outline"_s },
                     { u"visible"_s,
                       !this->chkShowBoundingBox || this->chkShowBoundingBox->isChecked() } });
    view->setState({ { u"type"_s, u"set_show_axes"_s },
                     { u"visible"_s, !this->chkShowAxes || this->chkShowAxes->isChecked() } });
    view->setState({ { u"type"_s, u"set_show_colorbar"_s }, { u"visible"_s, false } });
}

// ── A table of points (issue #83) ────────────────────────────────────────────

void vtkWindowCatalogue3D::applyPointTableLayout()
{
    if (!this->m_cartesian)
        return;
    // Positions are columns (a VBT, a simulation's CSV, a .speck): no frame to
    // rotate them into, no redshift to place them by, no shells around an
    // observer.
    if (this->m_sceneForm && this->comboFrame)
        this->m_sceneForm->setRowVisible(this->comboFrame, false);
    if (this->m_cosmoCard)
        this->m_cosmoCard->hide();
    if (this->chkShowShells)
        this->chkShowShells->hide();
    // Names and morphology only when the table has them.
    const bool names = !Catalogue3DParser::detail::detectedNameField(this->schema).isEmpty();
    const bool morphology =
            !Catalogue3DParser::detail::detectedMorphologyField(this->schema).isEmpty();
    this->m_noMorphology = !morphology;
    if (!morphology) {
        if (ui->chkMorphColors)
            ui->chkMorphColors->hide();
        if (this->comboMapColor && this->comboMapColor->count() > 0)
            this->comboMapColor->setItemText(0, u"Single colour"_s);
    }
    // The table lists the file's own columns: the computed sky position,
    // distance and scene X/Y/Z (copies of the position columns) mean nothing
    // here; the name and type, unless the table has them.
    if (this->catalogueTableView) {
        this->catalogueTableView->setColumnHidden(0, !names);
        this->catalogueTableView->setColumnHidden(1, !morphology);
        for (int column = 2; column < 8; ++column)
            this->catalogueTableView->setColumnHidden(column, true);
    }
}

void vtkWindowCatalogue3D::choosePointTableDefaults()
{
    if (!this->m_cartesian
        || !Catalogue3DParser::detail::detectedMorphologyField(this->schema).isEmpty())
        return;   // a catalogue with classes keeps its morphology colours
    // Points, or splats past a million rows (a simulation's particles), coloured
    // by the first quantity that is not a position or a velocity component.
    if (this->comboGeometry) {
        const int shape = this->comboGeometry->findText(
                this->m_tableRows > 1000000 ? u"Gaussian splats"_s : u"Point"_s);
        if (shape >= 0)
            this->comboGeometry->setCurrentIndex(shape);
    }
    if (!this->comboMapColor)
        return;
    QStringList skip{ this->schema.xFieldName, this->schema.yFieldName, this->schema.zFieldName };
    for (const int i : this->m_velocityColIdx)
        if (i >= 0)
            skip << this->rawHeaders.value(i);
    int fallback = -1;
    for (int i = 0; i < this->comboMapColor->count(); ++i) {
        const QString key = this->comboMapColor->itemData(i).toString();
        if (!key.startsWith(u"raw:"_s))
            continue;
        const QString column = this->rawHeaders.value(key.mid(4).toInt());
        if (skip.contains(column, Qt::CaseInsensitive))
            continue;
        if (fallback < 0)
            fallback = i;
        break;
    }
    if (fallback >= 0)
        this->comboMapColor->setCurrentIndex(fallback);
}

void vtkWindowCatalogue3D::refreshColorbar()
{
    if (!this->m_colorbar)
        return;
    // Any quantity (a column, the speed); not the morphology classes.
    const bool coloured = this->mapColorKey != u"builtin:morphology"_s
            && (!ui->chkMorphColors || this->m_noMorphology || ui->chkMorphColors->isChecked());
    const double *range = this->valueLut->GetTableRange();
    if (!coloured || !(range[1] > range[0])) {
        this->m_colorbar->hide();
        return;
    }
    const double lo = range[0], hi = range[1];
    const auto mode = this->comboColorScale
            ? static_cast<LutScale::Mode>(std::clamp(this->comboColorScale->currentIndex(), 0, 4))
            : LutScale::Mode::Linear;
    const double gamma = this->spinColorGamma ? this->spinColorGamma->value() : 2.0;
    // The value at a fraction u of the bar: the colours of a Log table are
    // log-spaced; a Sqrt/Square/Power table holds at t the colour of t^exp.
    const auto valueAt = [&](double u) {
        if (mode == LutScale::Mode::Log)
            return lo * std::pow(hi / lo, u);
        return lo + (hi - lo) * std::pow(u, 1.0 / LutScale::exponent(mode, gamma));
    };
    QVector<QColor> colours;
    for (int i = 0; i < 256; ++i) {
        double rgb[3];
        this->valueLut->GetColor(valueAt(i / 255.0), rgb);
        colours << QColor::fromRgbF(rgb[0], rgb[1], rgb[2]);
    }
    QVector<QPair<double, QString>> ticks;
    bool exponent = false;
    for (const double u : { 0.0, 0.25, 0.5, 0.75, 1.0 }) {
        ticks << qMakePair(u, QString::number(valueAt(u), 'g', 3));
        exponent = exponent || ticks.back().second.contains(u'e');
    }
    if (exponent) {
        for (auto &tick : ticks)
            tick.second = QString::number(valueAt(tick.first), 'e', 2);
    }
    this->m_colorbar->setColours(colours);
    this->m_colorbar->setTicks(ticks);
    this->m_colorbar->setTitle(this->mapColorKey.startsWith(u"raw:"_s)
                                       ? this->rawHeaders.value(this->mapColorKey.mid(4).toInt())
                                       : (this->comboMapColor ? this->comboMapColor->currentText()
                                                              : QString()));
    this->m_colorbar->setThreshold(std::nullopt);
    this->m_colorbar->show();
}

void vtkWindowCatalogue3D::showLutEditor()
{
    if (this->mapColorKey == u"builtin:morphology"_s) {
        this->statusBar()->showMessage(u"Colour by a quantity first (Color)."_s, 5000);
        return;
    }
    // The histogram is drawn from the colour values of the rows loaded.
    const vtkIdType n = this->morphScalars->GetNumberOfTuples();
    if (n <= 0)
        return;
    this->m_lutEditorData = vtkSmartPointer<vtkImageData>::New();
    this->m_lutEditorData->SetDimensions(static_cast<int>(n), 1, 1);
    vtkNew<vtkFloatArray> values;
    values->DeepCopy(this->morphScalars);
    this->m_lutEditorData->GetPointData()->SetScalars(values);
    if (!this->m_lutEditor) {
        this->m_lutEditor = new LUTCustomizerDialog(this);
        QObject::connect(this->m_lutEditor, &LUTCustomizerDialog::lutUpdated, this, [this]() {
            // The editor rebuilt valueLut: keep its map, scale and range as the
            // panel's state, then draw (and send) with them.
            const double *range = this->valueLut->GetTableRange();
            this->m_colorUserRange[0] = range[0];
            this->m_colorUserRange[1] = range[1];
            if (this->comboColormap) {
                const QSignalBlocker block(this->comboColormap);
                const int i = this->comboColormap->findData(
                        QString::fromStdString(this->valueLut->GetObjectName()));
                if (i >= 0)
                    this->comboColormap->setCurrentIndex(i);
            }
            if (this->comboColorScale) {
                const QSignalBlocker block(this->comboColorScale);
                this->comboColorScale->setCurrentIndex(
                        static_cast<int>(this->m_lutEditor->scaleMode()));
            }
            if (this->spinColorGamma) {
                const QSignalBlocker block(this->spinColorGamma);
                this->spinColorGamma->setValue(this->m_lutEditor->scaleGamma());
            }
            this->updateColorMapping();
            this->renderWindow->Render();
        });
    }
    const auto mode = this->comboColorScale
            ? static_cast<LutScale::Mode>(std::clamp(this->comboColorScale->currentIndex(), 0, 4))
            : LutScale::Mode::Linear;
    this->m_lutEditor->init(this->m_lutEditorData, this->valueLut, mode,
                            this->spinColorGamma ? this->spinColorGamma->value() : 2.0);
    this->m_lutEditor->show();
    this->m_lutEditor->raise();
    this->m_lutEditor->activateWindow();
}
