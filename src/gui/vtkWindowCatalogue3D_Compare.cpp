#include "vtkWindowCatalogue3D.h"

#include "CatalogueSelectionKey.h"
#include "LinkedWindowRegistry.h"
#include "RaytraceViewport.h"
#include "app/DatasetName.h"
#include "ui_vtkWindowCatalogue3D.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QJsonArray>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QSet>
#include <QSignalBlocker>
#include <QSlider>
#include <QStatusBar>
#include <QTimer>
#include <QUuid>
#include <QtConcurrent>

#include <vtkCallbackCommand.h>
#include <vtkCamera.h>
#include <vtkCommand.h>
#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkRenderer.h>
#include <vtkRenderWindow.h>

#include <algorithm>
#include <cmath>
#include <limits>
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
    if (this->m_comparisonRepresentationLinked)
        peer->applyComparisonRepresentation(this->comparisonRepresentation());

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
        this->m_comparisonRepresentationLinked = source->m_comparisonRepresentationLinked;
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
    if (this->comboGeometry)
        QObject::connect(this->comboGeometry, &QComboBox::currentIndexChanged, this,
                         [this]() { this->broadcastComparisonRepresentation(); });
    if (ui->sliderScale)
        QObject::connect(ui->sliderScale, &QSlider::valueChanged, this,
                         [this]() { this->broadcastComparisonRepresentation(); });
    if (this->comboRenderBudget)
        QObject::connect(this->comboRenderBudget, &QComboBox::currentIndexChanged, this,
                         [this]() { this->broadcastComparisonRepresentation(); });
    if (this->m_rtLightCheck)
        QObject::connect(this->m_rtLightCheck, &QCheckBox::toggled, this,
                         [this]() { this->broadcastComparisonRepresentation(); });
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
    const bool inGroup = !this->m_comparisonGroup.isEmpty();
    const bool linked = this->m_comparisonLinked && inGroup;
    if (this->m_comparisonCard)
        this->m_comparisonCard->setVisible(inGroup);
    if (this->m_comparisonLinkToggle) {
        const QSignalBlocker block(this->m_comparisonLinkToggle);
        this->m_comparisonLinkToggle->setChecked(linked);
        this->m_comparisonLinkToggle->setText(linked ? u"Linked"_s : u"Unlinked"_s);
    }
    if (this->m_comparisonRepresentationToggle) {
        const QSignalBlocker block(this->m_comparisonRepresentationToggle);
        this->m_comparisonRepresentationToggle->setChecked(
                this->m_comparisonRepresentationLinked);
        this->m_comparisonRepresentationToggle->setEnabled(linked);
    }
    if (!inGroup) {
        this->updateWindowTitle();
        return;
    }

    int views = 0;
    const auto windows = comparisonWindows().snapshot();
    for (const QPointer<vtkWindowCatalogue3D> &window : windows) {
        if (window && window->m_comparisonGroup == this->m_comparisonGroup)
            ++views;
    }
    const QString quantity = this->comboMapColor
            ? this->comboMapColor->currentText()
            : u"Not selected"_s;
    if (this->m_comparisonSummary) {
        this->m_comparisonSummary->setText(
                u"<b>%1 · %2 VIEWS</b><br>"
                 "Quantity in this view: <b>%3</b><br>"
                 "<small>Linked: camera and selection%4<br>"
                 "Independent: quantity, colormap, colour range and stretch</small>"_s
                        .arg(linked ? u"LINKED COMPARISON"_s : u"UNLINKED VIEW"_s)
                        .arg(views)
                        .arg(quantity.toHtmlEscaped())
                        .arg(this->m_comparisonRepresentationLinked
                                     ? u", representation"_s
                                     : u"; rendering is autonomous"_s));
    }
    this->updateWindowTitle();
}

vtkWindowCatalogue3D::ComparisonRepresentation
vtkWindowCatalogue3D::comparisonRepresentation() const
{
    ComparisonRepresentation state;
    if (this->comboGeometry)
        state.geometry = this->comboGeometry->currentText();
    if (ui->sliderScale)
        state.sizeSlider = ui->sliderScale->value();
    if (this->comboRenderBudget)
        state.renderBudget = this->comboRenderBudget->currentData().toInt();
    state.raytraced = this->m_rtLightCheck && this->m_rtLightCheck->isChecked();
    return state;
}

void vtkWindowCatalogue3D::applyComparisonRepresentation(
        const ComparisonRepresentation &state)
{
    const QScopedValueRollback<bool> applying(this->m_applyingComparisonUpdate, true);
    if (this->comboGeometry && !state.geometry.isEmpty()) {
        const int item = this->comboGeometry->findText(state.geometry);
        if (item >= 0)
            this->comboGeometry->setCurrentIndex(item);
    }
    if (ui->sliderScale)
        ui->sliderScale->setValue(state.sizeSlider);
    if (this->comboRenderBudget) {
        const int item = this->comboRenderBudget->findData(state.renderBudget);
        if (item >= 0)
            this->comboRenderBudget->setCurrentIndex(item);
    }
    if (this->m_rtLightCheck)
        this->m_rtLightCheck->setChecked(state.raytraced);
}

void vtkWindowCatalogue3D::broadcastComparisonRepresentation()
{
    if (!this->m_comparisonLinked || !this->m_comparisonRepresentationLinked
        || this->m_comparisonGroup.isEmpty()
        || this->m_applyingComparisonUpdate)
        return;
    const ComparisonRepresentation state = this->comparisonRepresentation();
    const auto windows = comparisonWindows().snapshot();
    for (const QPointer<vtkWindowCatalogue3D> &window : windows) {
        if (window && window != this && window->m_comparisonLinked
            && window->m_comparisonRepresentationLinked
            && window->m_comparisonGroup == this->m_comparisonGroup)
            window->applyComparisonRepresentation(state);
    }
}

void vtkWindowCatalogue3D::setComparisonLinked(bool linked)
{
    if (this->m_comparisonGroup.isEmpty())
        return;
    if (!linked) {
        this->m_comparisonLinked = false;
        this->m_comparisonCameraTracker.refit(this->comparisonCameraPose());
        this->updateComparisonUi();
        this->statusBar()->showMessage(
                u"View unlinked: camera, selection and representation are now autonomous."_s,
                6000);
        return;
    }

    vtkWindowCatalogue3D *reference = nullptr;
    const auto windows = comparisonWindows().snapshot();
    for (const QPointer<vtkWindowCatalogue3D> &window : windows) {
        if (window && window != this && window->m_comparisonGroup == this->m_comparisonGroup
            && window->m_comparisonLinked) {
            reference = window;
            break;
        }
    }
    // If every member was unlinked, the first peer becomes the reference so a
    // single click reconnects the comparison instead of waiting for two clicks.
    if (!reference) {
        for (const QPointer<vtkWindowCatalogue3D> &window : windows) {
            if (window && window != this
                && window->m_comparisonGroup == this->m_comparisonGroup) {
                reference = window;
                reference->m_comparisonLinked = true;
                reference->updateComparisonUi();
                break;
            }
        }
    }
    this->m_comparisonLinked = true;
    if (reference) {
        if (this->m_comparisonRepresentationLinked)
            this->applyComparisonRepresentation(reference->comparisonRepresentation());
        this->applyComparisonCamera(reference->comparisonCameraPose());
        if (reference->m_rtView && !reference->m_rtView->camera().isEmpty())
            this->applyComparisonRaytraceCamera(reference->m_rtView->camera());
        const CatalogueSelection::Key selected = CatalogueSelection::keyOf(
                reference->entries, reference->schema, reference->selectedIndex);
        const int index = CatalogueSelection::indexOf(this->entries, this->schema, selected);
        const QScopedValueRollback<bool> applying(this->m_applyingComparisonUpdate, true);
        this->setSelectedSource(index);
    }
    this->m_comparisonCameraTracker.adopt(this->comparisonCameraPose());
    this->updateComparisonUi();
    this->statusBar()->showMessage(
            this->m_comparisonRepresentationLinked
                    ? u"View linked: camera, selection and representation synchronized. "
                       "Quantity and colour mapping remain independent."_s
                    : u"View linked: camera and selection synchronized; rendering remains autonomous."_s,
            8000);
}

void vtkWindowCatalogue3D::setComparisonRepresentationLinked(bool linked)
{
    this->m_comparisonRepresentationLinked = linked;
    if (linked && this->m_comparisonLinked && !this->m_comparisonGroup.isEmpty()) {
        const auto windows = comparisonWindows().snapshot();
        for (const QPointer<vtkWindowCatalogue3D> &window : windows) {
            if (window && window != this && window->m_comparisonLinked
                && window->m_comparisonRepresentationLinked
                && window->m_comparisonGroup == this->m_comparisonGroup) {
                this->applyComparisonRepresentation(window->comparisonRepresentation());
                break;
            }
        }
    }
    this->updateComparisonUi();
    this->statusBar()->showMessage(
            linked
                    ? u"Rendering matched: shape, size, budget and ray-traced light follow the group."_s
                    : u"Rendering autonomous: camera and selection remain linked."_s,
            7000);
}

void vtkWindowCatalogue3D::setupSnapshotTimeline(vtkWindowCatalogue3D *source)
{
    if (!this->m_pointTable || this->remoteDatasetId.isEmpty())
        return;
    if (source) {
        this->m_snapshotSeries = source->m_snapshotSeries;
        this->updateSnapshotTimelineUi();
        return;
    }

    QObject::connect(&this->m_snapshotSeriesWatcher,
                     &QFutureWatcher<BackendPointSeriesResult>::finished, this, [this]() {
        const BackendPointSeriesResult result = this->m_snapshotSeriesWatcher.result();
        const auto windows = comparisonWindows().snapshot();
        for (const QPointer<vtkWindowCatalogue3D> &window : windows) {
            if (!window)
                continue;
            if (window == this
                || (!this->m_comparisonGroup.isEmpty()
                    && window->m_comparisonGroup == this->m_comparisonGroup)) {
                window->m_snapshotSeries = result;
                window->updateSnapshotTimelineUi();
            }
        }
    });
    const QString url = this->backendUrl;
    const QString token = this->backendToken;
    const QString session = this->remoteSessionId;
    const QString dataset = this->remoteDatasetId;
    this->m_snapshotSeriesWatcher.setFuture(QtConcurrent::run(
            [url, token, session, dataset]() {
                BackendClient client(url, token);
                client.setSessionId(session);
                return client.requestPointSeries(dataset);
            }));
}

void vtkWindowCatalogue3D::updateSnapshotTimelineUi()
{
    const bool available = this->m_snapshotSeries.valid
            && this->m_snapshotSeries.paths.size() > 1
            && this->m_snapshotSeries.index >= 0;
    if (this->m_timelineCard)
        this->m_timelineCard->setVisible(available);
    if (!available)
        return;

    const int index = this->m_snapshotSeries.index;
    const int number = this->m_snapshotSeries.numbers.value(index);
    QString details = u"Output %1 · %2 of %3"_s
            .arg(number).arg(index + 1).arg(this->m_snapshotSeries.count);
    const double time = this->m_snapshotMetadata.value(u"time"_s).toDouble(
            std::numeric_limits<double>::quiet_NaN());
    const double redshift = this->m_snapshotMetadata.value(u"redshift"_s).toDouble(
            std::numeric_limits<double>::quiet_NaN());
    if (std::isfinite(time))
        details += u" · time %1"_s.arg(time, 0, 'g', 5);
    if (std::isfinite(redshift) && redshift >= 0.0)
        details += u" · z %1"_s.arg(redshift, 0, 'g', 5);
    details += u"\nChanging output clears source selection; it does not track particles."_s;
    if (this->m_timelineSummary)
        this->m_timelineSummary->setText(details);
    if (this->m_timelineSlider) {
        const QSignalBlocker block(this->m_timelineSlider);
        this->m_timelineSlider->setRange(0, this->m_snapshotSeries.paths.size() - 1);
        this->m_timelineSlider->setValue(index);
        this->m_timelineSlider->setEnabled(!this->m_snapshotSwitching);
    }
    if (this->m_timelinePrevious)
        this->m_timelinePrevious->setEnabled(!this->m_snapshotSwitching && index > 0);
    if (this->m_timelineNext)
        this->m_timelineNext->setEnabled(
                !this->m_snapshotSwitching && index + 1 < this->m_snapshotSeries.paths.size());
    if (this->m_timelinePlay) {
        this->m_timelinePlay->setText(this->m_snapshotPlaying ? u"Pause"_s : u"Play"_s);
        this->m_timelinePlay->setEnabled(!this->m_snapshotSwitching);
    }
}

void vtkWindowCatalogue3D::stepSnapshot(int delta)
{
    if (!this->m_snapshotSeries.valid || this->m_snapshotSwitching)
        return;
    const int target = this->m_snapshotSeries.index + delta;
    if (target < 0 || target >= this->m_snapshotSeries.paths.size()) {
        if (this->m_snapshotPlaying)
            this->toggleSnapshotPlayback();
        return;
    }
    this->switchSnapshotIndex(target);
}

void vtkWindowCatalogue3D::toggleSnapshotPlayback()
{
    const bool playing = !this->m_snapshotPlaying;
    const auto windows = comparisonWindows().snapshot();
    for (const QPointer<vtkWindowCatalogue3D> &window : windows) {
        if (!window)
            continue;
        if (window == this
            || (!this->m_comparisonGroup.isEmpty()
                && window->m_comparisonGroup == this->m_comparisonGroup)) {
            window->m_snapshotPlaying = playing;
            if (window->m_timelineTimer)
                window->m_timelineTimer->stop();
            window->updateSnapshotTimelineUi();
        }
    }
    if (playing && this->m_timelineTimer)
        this->m_timelineTimer->start();
}

void vtkWindowCatalogue3D::switchSnapshotIndex(int index)
{
    if (!this->m_snapshotSeries.valid || this->m_snapshotSwitching
        || index == this->m_snapshotSeries.index || index < 0
        || index >= this->m_snapshotSeries.paths.size())
        return;

    struct ViewState {
        QPointer<vtkWindowCatalogue3D> old;
        QString colourBy;
        QString colormap;
        int colourScale{ 0 };
        double gamma{ 2.0 };
        int geometry{ 0 };
        int renderBudget{ 1000000 };
        bool raytraced{ false };
        bool linked{ true };
        bool representationLinked{ true };
        LinkedCameraSync::Pose camera;
        QJsonObject rayCamera;
        QRect windowGeometry;
    };
    QList<ViewState> states;
    const auto registered = comparisonWindows().snapshot();
    for (const QPointer<vtkWindowCatalogue3D> &window : registered) {
        if (!window)
            continue;
        const bool same = window == this
                || (!this->m_comparisonGroup.isEmpty()
                    && window->m_comparisonGroup == this->m_comparisonGroup);
        if (!same)
            continue;
        if (window->m_snapshotSwitching)
            return;
        ViewState state;
        state.old = window;
        state.colourBy = window->comboMapColor
                ? window->comboMapColor->currentData().toString() : QString();
        state.colormap = window->comboColormap
                ? window->comboColormap->currentData().toString() : QString();
        state.colourScale = window->comboColorScale
                ? window->comboColorScale->currentIndex() : 0;
        state.gamma = window->spinColorGamma ? window->spinColorGamma->value() : 2.0;
        state.geometry = window->comboGeometry ? window->comboGeometry->currentIndex() : 0;
        state.renderBudget = window->comboRenderBudget
                ? window->comboRenderBudget->currentData().toInt() : 1000000;
        state.raytraced = window->m_rtLightCheck && window->m_rtLightCheck->isChecked();
        state.linked = window->m_comparisonLinked;
        state.representationLinked = window->m_comparisonRepresentationLinked;
        state.camera = window->comparisonCameraPose();
        state.rayCamera = window->m_rtView ? window->m_rtView->camera() : QJsonObject{};
        state.windowGeometry = window->geometry();
        states.append(state);
    }
    if (states.isEmpty())
        return;

    const bool resumePlayback = std::any_of(
            states.cbegin(), states.cend(), [](const ViewState &state) {
                return state.old && state.old->m_snapshotPlaying;
            });
    for (ViewState &state : states) {
        state.old->m_snapshotSwitching = true;
        state.old->m_snapshotPlaying = false;
        if (state.old->m_timelineTimer)
            state.old->m_timelineTimer->stop();
        state.old->updateSnapshotTimelineUi();
    }

    const QString targetPath = this->m_snapshotSeries.paths.at(index);
    BackendPointSeriesResult targetSeries = this->m_snapshotSeries;
    targetSeries.index = index;
    const QString particle = this->m_componentMetadata.value(u"name"_s).toString();
    if (!particle.isEmpty())
        visivo::rememberFieldNames({ { particle, targetPath } });
    const Catalogue3DLabel label{ visivo::datasetDisplayName(targetPath), false };

    auto *primary = new vtkWindowCatalogue3D(
            targetPath, this->backendUrl, this->backendToken, {}, label,
            this->parentWidget(), nullptr);
    primary->setAttribute(Qt::WA_DeleteOnClose);
    if (!primary->catalogueOpened()) {
        primary->deleteLater();
        for (ViewState &state : states) {
            if (!state.old)
                continue;
            state.old->m_snapshotSwitching = false;
            state.old->m_snapshotPlaying = resumePlayback;
            state.old->updateSnapshotTimelineUi();
        }
        if (resumePlayback && this->m_timelineTimer)
            this->m_timelineTimer->start();
        return;
    }

    const auto restore = [](vtkWindowCatalogue3D *window, const ViewState &state) {
        if (window->comboMapColor) {
            const int item = window->comboMapColor->findData(state.colourBy);
            if (item >= 0)
                window->comboMapColor->setCurrentIndex(item);
        }
        if (window->comboColormap) {
            const int item = window->comboColormap->findData(state.colormap);
            if (item >= 0)
                window->comboColormap->setCurrentIndex(item);
        }
        if (window->comboColorScale)
            window->comboColorScale->setCurrentIndex(state.colourScale);
        if (window->spinColorGamma)
            window->spinColorGamma->setValue(state.gamma);
        if (window->comboGeometry)
            window->comboGeometry->setCurrentIndex(state.geometry);
        if (window->comboRenderBudget) {
            const int item = window->comboRenderBudget->findData(state.renderBudget);
            if (item >= 0)
                window->comboRenderBudget->setCurrentIndex(item);
        }
        if (window->m_rtLightCheck && state.raytraced)
            window->m_rtLightCheck->setChecked(true);
        window->setGeometry(state.windowGeometry);
    };

    primary->m_snapshotSeries = targetSeries;
    primary->updateSnapshotTimelineUi();
    restore(primary, states.first());
    primary->m_comparisonLinked = states.first().linked;
    primary->m_comparisonRepresentationLinked = states.first().representationLinked;
    QList<QPointer<vtkWindowCatalogue3D>> replacements{ primary };
    if (states.size() > 1) {
        primary->m_comparisonGroup = QUuid::createUuid().toString(QUuid::WithoutBraces);
        primary->m_comparisonLinked = states.first().linked;
        for (int i = 1; i < states.size(); ++i) {
            auto *peer = new vtkWindowCatalogue3D(
                    targetPath, this->backendUrl, this->backendToken, {}, label,
                    states.at(i).old ? states.at(i).old->parentWidget() : this->parentWidget(),
                    primary);
            peer->setAttribute(Qt::WA_DeleteOnClose);
            peer->m_snapshotSeries = targetSeries;
            restore(peer, states.at(i));
            peer->m_comparisonLinked = states.at(i).linked;
            peer->m_comparisonRepresentationLinked = states.at(i).representationLinked;
            replacements.append(peer);
        }
    }
    for (const QPointer<vtkWindowCatalogue3D> &window : replacements) {
        if (!window)
            continue;
        window->m_snapshotSwitching = false;
        const int replacementIndex = replacements.indexOf(window);
        if (replacementIndex >= 0) {
            const ViewState &state = states.at(replacementIndex);
            window->applyComparisonCamera(state.camera);
            if (!state.rayCamera.isEmpty())
                window->applyComparisonRaytraceCamera(state.rayCamera);
        }
        window->updateComparisonUi();
        window->updateSnapshotTimelineUi();
        window->show();
        window->raise();
    }
    primary->statusBar()->showMessage(
            u"Snapshot changed for every linked view. Source selection was cleared; "
             "no cross-snapshot particle identity was assumed."_s,
            10000);

    for (ViewState &state : states) {
        if (state.old)
            state.old->close();
    }
    if (resumePlayback && index + 1 < targetSeries.paths.size()) {
        primary->m_snapshotPlaying = true;
        primary->updateSnapshotTimelineUi();
        if (primary->m_timelineTimer)
            primary->m_timelineTimer->start();
    }
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
