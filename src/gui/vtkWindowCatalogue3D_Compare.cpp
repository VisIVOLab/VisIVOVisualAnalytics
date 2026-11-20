#include "vtkWindowCatalogue3D.h"

#include "CatalogueSelectionKey.h"
#include "LinkedWindowRegistry.h"
#include "RaytraceViewport.h"

#include <QComboBox>
#include <QJsonArray>
#include <QLabel>
#include <QSet>
#include <QStatusBar>
#include <QTimer>
#include <QUuid>

#include <vtkCallbackCommand.h>
#include <vtkCamera.h>
#include <vtkCommand.h>
#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkRenderer.h>
#include <vtkRenderWindow.h>

#include <utility>

using namespace Qt::StringLiterals;

namespace {

LinkedWindowRegistry<vtkWindowCatalogue3D> &comparisonWindows()
{
    static LinkedWindowRegistry<vtkWindowCatalogue3D> windows;
    return windows;
}

} // namespace

void vtkWindowCatalogue3D::openQuantityComparison()
{
    if (!this->m_pointTable || this->remoteDatasetId.isEmpty())
        return;
    if (this->filterBusy || this->m_sceneBuilding) {
        this->statusBar()->showMessage(
                u"Wait for the current particle load to finish before opening a comparison."_s,
                6000);
        return;
    }

    if (this->m_comparisonGroup.isEmpty())
        this->m_comparisonGroup = QUuid::createUuid().toString(QUuid::WithoutBraces);
    this->m_comparisonLinked = true;
    this->m_comparisonCameraTracker.refit(this->comparisonCameraPose());
    this->updateWindowTitle();

    auto *peer = new vtkWindowCatalogue3D(
            this->filepath, this->backendUrl, this->backendToken, this->m_cataloguePaths,
            this->m_label, this->parentWidget(), this);
    peer->setAttribute(Qt::WA_DeleteOnClose);
    peer->setWindowFlag(Qt::Window, true);
    peer->resize(this->size());
    peer->move(this->pos() + QPoint(36, 36));
    peer->show();
    peer->raise();

    // Prefer the three quantities proposed for the MVP, then other physical
    // scalars. Repeating the action therefore opens density, temperature and
    // entropy when they exist instead of producing several rho views.
    QSet<QString> used;
    const auto group = comparisonWindows().snapshot();
    for (const QPointer<vtkWindowCatalogue3D> &window : group) {
        if (window && window != peer && window->m_comparisonGroup == this->m_comparisonGroup
            && window->comboMapColor)
            used.insert(window->comboMapColor->currentData().toString());
    }
    const QStringList preferred{ u"rho"_s, u"density"_s, u"temperature"_s, u"temp"_s,
                                 u"entropy"_s, u"k"_s, u"u"_s, u"pressure"_s,
                                 u"metallicity"_s, u"mass"_s, u"sfr"_s, u"age"_s };
    int quantity = -1;
    const auto usable = [peer, &used](int item) {
        const QString key = peer->comboMapColor->itemData(item).toString();
        if (!key.startsWith(u"raw:"_s) || used.contains(key))
            return false;
        const QString name = peer->rawHeaders.value(key.mid(4).toInt());
        const QStringList coordinates{ peer->schema.xFieldName, peer->schema.yFieldName,
                                       peer->schema.zFieldName, u"vx"_s, u"vy"_s, u"vz"_s };
        return !coordinates.contains(name, Qt::CaseInsensitive);
    };
    for (const QString &wanted : preferred) {
        for (int i = 0; i < peer->comboMapColor->count(); ++i) {
            if (!usable(i))
                continue;
            const QString key = peer->comboMapColor->itemData(i).toString();
            if (peer->rawHeaders.value(key.mid(4).toInt()).compare(
                        wanted, Qt::CaseInsensitive) == 0) {
                quantity = i;
                break;
            }
        }
        if (quantity >= 0)
            break;
    }
    for (int i = 0; quantity < 0 && i < peer->comboMapColor->count(); ++i)
        if (usable(i))
            quantity = i;
    if (quantity >= 0)
        peer->comboMapColor->setCurrentIndex(quantity);

    // The count and quantity are persistent in every member of the group. This
    // is deliberately stronger than the transient status-bar message below.
    const auto updatedGroup = comparisonWindows().snapshot();
    for (const QPointer<vtkWindowCatalogue3D> &window : updatedGroup) {
        if (window && window->m_comparisonGroup == this->m_comparisonGroup)
            window->updateComparisonUi();
    }

    // The constructor builds the scene before it can join the group. Give the
    // new view the already-framed camera now, so it opens on precisely the
    // same region rather than briefly asking the user to align it by hand.
    peer->applyComparisonCamera(this->comparisonCameraPose());
    if (this->m_rtView && !this->m_rtView->camera().isEmpty())
        peer->applyComparisonRaytraceCamera(this->m_rtView->camera());
    if (this->selectedIndex >= 0)
        this->broadcastComparisonSelection(this->selectedIndex);

    this->statusBar()->showMessage(
            u"Comparison opened: camera and source selection are linked; colour, range and "
            u"scale remain independent."_s,
            8000);
}

void vtkWindowCatalogue3D::setupComparisonLink(vtkWindowCatalogue3D *source)
{
    comparisonWindows().add(this);
    if (source) {
        this->m_comparisonGroup = source->m_comparisonGroup;
        this->m_comparisonLinked = true;
    }
    this->updateComparisonUi();

    vtkNew<vtkCallbackCommand> callback;
    callback->SetClientData(this);
    callback->SetCallback([](vtkObject *, unsigned long, void *data, void *) {
        static_cast<vtkWindowCatalogue3D *>(data)->onComparisonCameraFrame();
    });
    this->m_comparisonCameraObserverTag =
            this->renderWindow->AddObserver(vtkCommand::EndEvent, callback);
    this->m_comparisonCameraTracker.refit(this->comparisonCameraPose());

    QObject::connect(this, &vtkWindowCatalogue3D::sourceSelectionChanged, this,
                     &vtkWindowCatalogue3D::broadcastComparisonSelection);
}

void vtkWindowCatalogue3D::teardownComparisonLink()
{
    const QString group = this->m_comparisonGroup;
    if (this->m_comparisonCameraObserverTag && this->renderWindow) {
        this->renderWindow->RemoveObserver(this->m_comparisonCameraObserverTag);
        this->m_comparisonCameraObserverTag = 0;
    }
    comparisonWindows().remove(this);
    if (!group.isEmpty()) {
        const auto remaining = comparisonWindows().snapshot();
        int groupSize = 0;
        vtkWindowCatalogue3D *last = nullptr;
        for (const QPointer<vtkWindowCatalogue3D> &window : remaining) {
            if (window && window->m_comparisonGroup == group) {
                ++groupSize;
                last = window;
            }
        }
        // A comparison needs at least two views. Do not leave the last window
        // claiming to be linked to a peer that no longer exists.
        if (groupSize == 1 && last) {
            last->m_comparisonGroup.clear();
            last->m_comparisonLinked = false;
        }
        for (const QPointer<vtkWindowCatalogue3D> &window : remaining) {
            if (window && (window->m_comparisonGroup == group || window == last))
                window->updateComparisonUi();
        }
    }
}

void vtkWindowCatalogue3D::updateComparisonUi()
{
    const bool linked = this->m_comparisonLinked && !this->m_comparisonGroup.isEmpty();
    if (this->m_comparisonCard)
        this->m_comparisonCard->setVisible(linked);
    if (!linked) {
        this->updateWindowTitle();
        return;
    }

    int views = 0;
    const auto windows = comparisonWindows().snapshot();
    for (const QPointer<vtkWindowCatalogue3D> &window : windows) {
        if (window && window->m_comparisonLinked
            && window->m_comparisonGroup == this->m_comparisonGroup)
            ++views;
    }
    const QString quantity = this->comboMapColor
            ? this->comboMapColor->currentText()
            : u"Not selected"_s;
    if (this->m_comparisonSummary) {
        this->m_comparisonSummary->setText(
                u"<b>LINKED COMPARISON · %1 VIEWS</b><br>"
                 "Quantity in this view: <b>%2</b><br>"
                 "<small>Linked: camera and source selection<br>"
                 "Independent: quantity, colormap, range and scale</small>"_s
                        .arg(views)
                        .arg(quantity.toHtmlEscaped()));
    }
    this->updateWindowTitle();
}

LinkedCameraSync::Pose vtkWindowCatalogue3D::comparisonCameraPose() const
{
    LinkedCameraSync::Pose pose;
    vtkCamera *camera = this->renderer->GetActiveCamera();
    const double *position = camera->GetPosition();
    const double *focal = camera->GetFocalPoint();
    const double *up = camera->GetViewUp();
    for (int i = 0; i < 3; ++i) {
        pose.position[i] = position[i];
        pose.focalPoint[i] = focal[i];
        pose.viewUp[i] = up[i];
    }
    pose.viewAngle = camera->GetViewAngle();
    pose.parallelScale = camera->GetParallelScale();
    return pose;
}

void vtkWindowCatalogue3D::onComparisonCameraFrame()
{
    const LinkedCameraSync::Pose pose = this->comparisonCameraPose();
    if (!this->m_comparisonLinked || this->m_comparisonGroup.isEmpty()) {
        this->m_comparisonCameraTracker.refit(pose);
        return;
    }
    if (!this->m_comparisonCameraTracker.frame(pose, this->m_applyingComparisonUpdate))
        return;

    // Coalesce the burst of EndEvents produced by a drag. The tracker makes
    // this one send carry the latest pose and prevents peer echo.
    QTimer::singleShot(0, this, [this]() {
        if (!this->m_comparisonCameraTracker.takePending() || !this->m_comparisonLinked)
            return;
        const LinkedCameraSync::Pose latest = this->comparisonCameraPose();
        this->m_comparisonCameraTracker.adopt(latest);
        const auto peers = comparisonWindows().snapshot();
        for (const QPointer<vtkWindowCatalogue3D> &peer : peers) {
            if (peer && peer != this && peer->m_comparisonLinked
                && peer->m_comparisonGroup == this->m_comparisonGroup)
                peer->applyComparisonCamera(latest);
        }
    });
}

void vtkWindowCatalogue3D::applyComparisonCamera(const LinkedCameraSync::Pose &pose)
{
    if (!pose.finite())
        return;
    this->m_applyingComparisonUpdate = true;
    vtkCamera *camera = this->renderer->GetActiveCamera();
    camera->SetPosition(pose.position.data());
    camera->SetFocalPoint(pose.focalPoint.data());
    camera->SetViewUp(pose.viewUp.data());
    camera->SetViewAngle(pose.viewAngle);
    if (pose.parallelScale > 0.0)
        camera->SetParallelScale(pose.parallelScale);
    this->renderer->ResetCameraClippingRange();
    this->m_comparisonCameraTracker.adopt(pose);
    this->renderWindow->Render();
    this->m_applyingComparisonUpdate = false;
}

void vtkWindowCatalogue3D::broadcastComparisonSelection(int index)
{
    if (!this->m_comparisonLinked || this->m_comparisonGroup.isEmpty()
        || this->m_applyingComparisonUpdate)
        return;
    const CatalogueSelection::Key key =
            CatalogueSelection::keyOf(this->entries, this->schema, index);
    const auto peers = comparisonWindows().snapshot();
    for (const QPointer<vtkWindowCatalogue3D> &peer : peers) {
        if (!peer || peer == this || !peer->m_comparisonLinked
            || peer->m_comparisonGroup != this->m_comparisonGroup)
            continue;
        const int peerIndex = index < 0
                ? -1
                : CatalogueSelection::indexOf(peer->entries, peer->schema, key);
        peer->m_applyingComparisonUpdate = true;
        const bool wasSamp = std::exchange(peer->m_applyingSampSelection, true);
        peer->setSelectedSource(peerIndex);
        peer->m_applyingSampSelection = wasSamp;
        peer->m_applyingComparisonUpdate = false;
    }
}

void vtkWindowCatalogue3D::broadcastComparisonRaytraceCamera(const QJsonObject &camera)
{
    if (!this->m_comparisonLinked || this->m_comparisonGroup.isEmpty()
        || this->m_applyingComparisonUpdate || camera.isEmpty())
        return;
    const auto peers = comparisonWindows().snapshot();
    for (const QPointer<vtkWindowCatalogue3D> &peer : peers) {
        if (peer && peer != this && peer->m_comparisonLinked
            && peer->m_comparisonGroup == this->m_comparisonGroup)
            peer->applyComparisonRaytraceCamera(camera);
    }
}

void vtkWindowCatalogue3D::applyComparisonRaytraceCamera(const QJsonObject &camera)
{
    if (camera.isEmpty())
        return;
    const auto value = [&camera](const QString &name, int component) {
        return camera.value(name).toArray().at(component).toDouble();
    };
    this->m_applyingComparisonUpdate = true;
    vtkCamera *vtkCamera = this->renderer->GetActiveCamera();
    vtkCamera->SetPosition(value(u"position"_s, 0), value(u"position"_s, 1),
                           value(u"position"_s, 2));
    vtkCamera->SetFocalPoint(value(u"focal_point"_s, 0), value(u"focal_point"_s, 1),
                            value(u"focal_point"_s, 2));
    vtkCamera->SetViewUp(value(u"view_up"_s, 0), value(u"view_up"_s, 1),
                        value(u"view_up"_s, 2));
    if (camera.contains(u"view_angle"_s))
        vtkCamera->SetViewAngle(camera.value(u"view_angle"_s).toDouble(30.0));
    if (camera.contains(u"parallel"_s))
        vtkCamera->SetParallelProjection(camera.value(u"parallel"_s).toBool() ? 1 : 0);
    if (camera.value(u"parallel_scale"_s).toDouble() > 0.0)
        vtkCamera->SetParallelScale(camera.value(u"parallel_scale"_s).toDouble());
    this->renderer->ResetCameraClippingRange();
    this->m_comparisonCameraTracker.adopt(this->comparisonCameraPose());
    if (this->m_rtView)
        this->m_rtView->setCamera(camera);
    this->renderWindow->Render();
    this->m_applyingComparisonUpdate = false;
}
