// "Remove from Session" in the image viewer (issue #580) — partial class of
// vtkWindowImage.
//
// Until now a layer, once loaded, stayed: an additional image, a Faraday map or
// any other derived map could only be hidden. The gesture (Delete key, context
// menu, multi-selection) and the question belong to SessionDataTree; this file
// says what a removal means for an image window: the layer leaves the stack and
// the layer list, its image is freed, the state this window keeps by layer row
// follows, and the product bound to the layer goes with it. The layer list
// reaches the same code, so the two cannot drift apart. A file the user opened
// is never deleted: a layer is only ever a view of it.

#include "vtkWindowImage.h"

#include <QDockWidget>
#include <QStatusBar>

#include "LayerListModel.h"   // src/, not src/gui/
#include "SessionDataTree.h"
#include "ToolResultDialog.h"
#include "WorkspaceChrome.h"
#include "app/ProductRegistry.h"
#include "app/SessionRemoval.h"
#include "ui_vtkWindowImage.h"

#include <QListView>
#include <algorithm>
#include <QMenu>
#include <QTimer>
#include <vtkImageSlice.h>
#include <vtkImageStack.h>

using namespace Qt::StringLiterals;

QString vtkWindowImage::productIdForLayer(int row) const
{
    if (row < 0)
        return {};
    for (auto it = m_productLayer.cbegin(); it != m_productLayer.cend(); ++it)
        if (this->productLayerRow(it.key()) == row)
            return it.key();
    return {};
}

bool vtkWindowImage::removeLayerRow(int row)
{
    if (!this->layers || row < 0 || row >= this->layers->rowCount())
        return false;
    if (row == this->layers->getMasterIndex()) {
        this->statusBar()->showMessage(
                u"The image this window was opened on cannot be removed: close the window "
                u"instead."_s,
                6000);
        return false;
    }

    // The products that ARE this layer, read before the rows move.
    QStringList bound;
    for (auto it = m_productLayer.cbegin(); it != m_productLayer.cend(); ++it)
        if (this->productLayerRow(it.key()) == row)
            bound << it.key();

    // Out of the stack first, while the actor is still the layer's.
    if (vtkImageSlice *actor = this->layers->getLayerActor(row))
        this->stack->RemoveImage(actor);

    // The state kept by layer ROW moves up with the layers below.
    if (static_cast<int>(this->savedLinearDisplayRanges.size()) > row)
        this->savedLinearDisplayRanges.erase(this->savedLinearDisplayRanges.begin() + row);
    QHash<int, StokesRole> roles;
    for (auto it = m_stokesRoles.cbegin(); it != m_stokesRoles.cend(); ++it) {
        if (it.key() < row)
            roles.insert(it.key(), it.value());
        else if (it.key() > row)
            roles.insert(it.key() - 1, it.value());
    }
    m_stokesRoles = roles;
    // The copilot's undo remembers which layer it changed: it follows, or
    // forgets, rather than undoing onto a neighbour.
    if (m_copilotUndo.valid) {
        if (m_copilotUndo.layerIndex == row) {
            m_copilotUndo.colormapCaptured = false;
            m_copilotUndo.scaleCaptured = false;
        } else if (m_copilotUndo.layerIndex > row) {
            --m_copilotUndo.layerIndex;
        }
    }
    for (auto it = m_productLayer.begin(); it != m_productLayer.end(); ++it)
        if (it.value() > row)
            --it.value();

    // The layer itself: its image is freed with it.
    const int before = this->layers->rowCount();
    if (!this->layers->removeLayer(row) || this->layers->rowCount() != before - 1)
        return false;

    // The additive blend keeps each layer's resampled pixels and holds its image:
    // a removed layer must not stay in it, in memory or in the next composite.
    this->dropBlendCache();

    // The product that was this layer has nothing left to show.
    for (const QString &id : std::as_const(bound)) {
        m_productLayer.remove(id);
        m_productLayerName.remove(id);
        m_productLayerActor.remove(id);
        if (auto *recorded = this->findChild<ToolResultDialog *>(u"recorded-result:"_s + id))
            recorded->close();
        ProductRegistry::instance().removeProduct(id);
    }

    // A layer is always selected: the one that took its place, or the one above.
    if (!ui->listLayer->currentIndex().isValid() && this->layers->rowCount() > 0)
        ui->listLayer->setCurrentIndex(
                this->layers->index(std::min(row, this->layers->rowCount() - 1), 0));

    if (this->m_workspaceChrome) {
        int mine = 0;
        for (const AnalysisProduct &ap : ProductRegistry::instance().productsFor(this->filepath))
            if (ap.ownerId == this->m_windowScope)
                ++mine;
        this->m_workspaceChrome->setSessionRailCount(mine);
    }
    // The stack's active layer and the colour bar follow the selection by ROW,
    // and the rows have just moved: bring them back in step.
    this->showCurrentLayerSettings();
    // The selection moved while the row was still counted (the model reports
    // the new current row before the removal ends): count the layers again.
    this->refreshUnitAndBeamRows();
    this->vtkRender();
    return true;
}

void vtkWindowImage::removeSessionProducts(const QStringList &productIds)
{
    int removed = 0;
    // A layer's row moves when a layer above it goes: each id is resolved
    // again at its turn rather than from a row read up front.
    for (const QString &id : productIds) {
        // Two products on one layer: the first removal took the other with it.
        if (ProductRegistry::instance().product(id).productId.isEmpty())
            continue;
        const int row = this->productLayerRow(id);
        if (row >= 0) {
            if (this->removeLayerRow(row))
                ++removed;
            continue;
        }
        // Not a layer: what the window kept to reopen it, and what it had open.
        m_extractedSpectra.remove(id);
        m_powerSpectra.remove(id);
        m_powerSpectrumFits.remove(id);
        m_skeletonMasks.remove(id);
        if (m_skeletonShownProduct == id)
            this->clearSkeletonOverlay();
        if (auto *recorded = this->findChild<ToolResultDialog *>(u"recorded-result:"_s + id))
            recorded->close();
        ProductRegistry::instance().removeProduct(id);
        ++removed;
    }
    if (removed > 0)
        this->statusBar()->showMessage(
                removed == 1 ? u"Removed from this session."_s
                             : u"Removed %1 items from this session."_s.arg(QString::number(removed)),
                4000);
}

void vtkWindowImage::requestLayerRemoval(int row)
{
    if (!this->layers || row < 0 || row >= this->layers->rowCount())
        return;
    if (row == this->layers->getMasterIndex()) {
        this->statusBar()->showMessage(
                u"The image this window was opened on cannot be removed: close the window "
                u"instead."_s,
                6000);
        return;
    }
    // A derived map is a Session Data product, and asks what any removal asks;
    // a layer that is just a file the user opened has nothing to lose.
    const QString productId = this->productIdForLayer(row);
    SessionDataTree *tree = this->m_workspaceChrome ? this->m_workspaceChrome->sessionTree()
                                                    : nullptr;
    if (!productId.isEmpty() && tree)
        tree->requestRemoval({ productId });
    else
        this->removeLayerRow(row);
}

int vtkWindowImage::layerRowOfActor(const vtkImageSlice *actor) const
{
    if (!actor || !this->layers)
        return -1;
    QList<const void *> actors;
    for (int r = 0; r < this->layers->rowCount(); ++r)
        actors << this->layers->getLayerActor(r);
    return visivo::removal::rowOfIdentity(actors, actor);
}

void vtkWindowImage::requestLayerRemovalLater(int row)
{
    if (!this->layers || row < 0 || row >= this->layers->rowCount())
        return;
    vtkWeakPointer<vtkImageSlice> actor = this->layers->getLayerActor(row);
    QTimer::singleShot(0, this, [this, actor]() {
        // Looked up again: another removal may have moved the row, or taken the
        // layer (two key presses before the first request ran).
        const int now = this->layerRowOfActor(actor.GetPointer());
        if (now >= 0)
            this->requestLayerRemoval(now);
    });
}

void vtkWindowImage::appendSessionRemoveAction(QMenu &menu, const QString &productId)
{
    if (SessionDataTree *tree = this->m_workspaceChrome ? this->m_workspaceChrome->sessionTree()
                                                        : nullptr)
        tree->appendRemoveAction(&menu, productId);
}

void vtkWindowImage::setupSessionRemoval()
{
    // Session Data: the same Delete key, menu entry and question as every viewer.
    if (SessionDataTree *tree = this->m_workspaceChrome ? this->m_workspaceChrome->sessionTree()
                                                        : nullptr) {
        visivo::removal::Handler handler;
        handler.refuse = [this](const AnalysisProduct &p) -> QString {
            // A product that is the image the window was opened on stays.
            const int row = this->productLayerRow(p.productId);
            if (row >= 0 && this->layers && row == this->layers->getMasterIndex())
                return u"it is the image this window was opened on"_s;
            return {};
        };
        handler.remove = [this](const QStringList &ids) { this->removeSessionProducts(ids); };
        tree->setRemovalHandler(std::move(handler));
    }

    // The layer list: right-click and Delete, ending in the same code.
    if (!ui->listLayer)
        return;
    ui->listLayer->installEventFilter(this);
    ui->listLayer->setContextMenuPolicy(Qt::CustomContextMenu);
    QObject::connect(ui->listLayer, &QWidget::customContextMenuRequested, this,
                     [this](const QPoint &pos) {
                         const QModelIndex idx = ui->listLayer->indexAt(pos);
                         if (!idx.isValid() || !this->layers)
                             return;
                         const int row = idx.row();
                         QMenu menu(this);
                         menu.setToolTipsVisible(true);
                         QAction *remove = menu.addAction(u"Remove Layer from Session"_s);
                         const bool master = row == this->layers->getMasterIndex();
                         remove->setEnabled(!master);
                         remove->setToolTip(
                                 master ? u"The image this window was opened on stays: close "
                                          u"the window to remove it."_s
                                        : u"Take this layer out of the window and free its "
                                          u"data. A file on disk is never deleted."_s);
                         QObject::connect(remove, &QAction::triggered, this,
                                          [this, row]() { this->requestLayerRemoval(row); });
                         menu.exec(ui->listLayer->viewport()->mapToGlobal(pos));
                     });
}
