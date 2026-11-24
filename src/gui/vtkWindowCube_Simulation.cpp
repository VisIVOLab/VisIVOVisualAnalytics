// vtkWindowCube_Simulation.cpp
// The cube window's side of a simulation volume (issue #524): the slice taken
// along x, y or z at a coordinate in the grid's units, and the physical units
// (UNIT_DENSITY, UNIT_LENGTH, UNIT_VELOCITY) the values, coordinates and time
// are shown in. The rules are in app/SimulationVolume.h; the backend serves
// the planes (/v1/cube/slice, axis) and the values already in the units asked
// for (value_scale); this is the window around them.

#include "vtkWindowCube.h"
#include "ui_vtkWindowCube.h"

#include "ChannelOverlay.h"
#include "InspectorPanel.h"
#include "PhysicalUnitsControl.h"
#include "app/BackendClient.h"

#include <QComboBox>
#include <QDoubleValidator>
#include <QLabel>
#include <QLineEdit>
#include <QSignalBlocker>
#include <QStatusBar>

#include <algorithm>
#include <cmath>

using namespace Qt::StringLiterals;
using visivo::simvol::SliceAxis;

bool vtkWindowCube::detectSimulationVolume() const
{
    // The three axes named X, Y and Z: the grid of a simulation (a PLUTO
    // variable, a VBT field). A spectral cube has a sky pair and a spectral
    // axis, and an empty or unknown type keeps every tool as it always has.
    std::array<bool, 3> seen{ false, false, false };
    for (const QString &raw : this->remoteDatasetCtype) {
        const QString t = raw.trimmed().toUpper();
        const int i = t == u"X"_s ? 0 : (t == u"Y"_s ? 1 : (t == u"Z"_s ? 2 : -1));
        if (i < 0 || seen[std::size_t(i)])
            return false;
        seen[std::size_t(i)] = true;
    }
    return true;
}

std::array<int, 2> vtkWindowCube::slicePlaneAxes() const
{
    return visivo::simvol::planeAxes(m_sliceAxis);
}

void vtkWindowCube::refreshAxisUnits()
{
    m_axisUnits = AxisUnitsChoice();
    if (!m_simulationVolume)
        return;
    for (int axis = 0; axis < 3; ++axis) {
        const QString column = visivo::units::axisColumn(axis);
        m_axisUnits.factor[std::size_t(axis)] =
                visivo::units::columnScale(m_physicalUnits, column, m_showPhysical);
        m_axisUnits.unit[std::size_t(axis)] = visivo::units::columnUnitLabel(
                m_physicalUnits, column, m_showPhysical, this->remoteDatasetCunit[axis].trimmed());
    }
}

QString vtkWindowCube::axisUnitLabel(int cubeAxis) const
{
    if (cubeAxis < 0 || cubeAxis >= 3)
        return QString();
    return m_simulationVolume ? m_axisUnits.unit[std::size_t(cubeAxis)]
                              : this->remoteDatasetCunit[cubeAxis].trimmed();
}

QString vtkWindowCube::sliceCoordinateText(int index) const
{
    const int axis = static_cast<int>(m_sliceAxis);
    const int clamped = this->clampRemoteSliceIndex(index);
    // remoteVoxelToWcs is in the units shown, the length factor included.
    bool ok = false;
    const double world = this->remoteVoxelToWcs(axis, double(clamped), &ok);
    if (!ok)
        return u"%1 = %2"_s.arg(visivo::simvol::axisLetter(m_sliceAxis)).arg(clamped + 1);
    return visivo::simvol::coordinateLabel(m_sliceAxis, world, this->axisUnitLabel(axis));
}

void vtkWindowCube::setPhysicalUnits(const visivo::units::PhysicalUnits &units)
{
    m_physicalUnits = units;
    m_showPhysical = units.hasValue() || units.hasLength();
    this->refreshAxisUnits();
    BackendClient::setDisplayValueScale(
            this->remoteDatasetId,
            visivo::units::columnScale(units, visivo::units::valueColumn(), m_showPhysical));
    // The unit the values carry follows (the header's own is kept for code units).
    this->setIntensityUnit(m_codeBunit, m_intensityUnitNote);
    this->refreshSimulationRows();
    if (m_simulationVolume)
        this->syncSliceAxisUi();
}

void vtkWindowCube::setSliceAxis(SliceAxis axis)
{
    if (!m_simulationVolume || axis == m_sliceAxis)
        return;
    // Another plane means another detail window and another set of planes in
    // flight; the cache is keyed by axis, so what was read stays valid.
    this->hideSliceDetail();
    m_sliceAxis = axis;
    // The old plane's index and grid describe nothing on the new axis: no
    // detail request may use them before the first plane arrives.
    this->m_displayedSliceIndex = -1;
    const int middle = this->remoteSliceCount() / 2;
    this->currentRequestedRemoteSliceIndex = middle;
    {
        const QSignalBlocker blockSlider(ui->sliderSlice);
        const QSignalBlocker blockSpin(ui->spinSlice);
        ui->sliderSlice->setMaximum(this->remoteSliceCount());
        ui->spinSlice->setMaximum(this->remoteSliceCount());
        ui->sliderSlice->setValue(middle + 1);
        ui->spinSlice->setValue(middle + 1);
    }
    this->syncSliceAxisUi();
    // The tools that read cube pixels follow the axis (the gate), and the
    // plane in the 3-D view moves to it.
    this->applyMomentToolGate();
    this->requestRemoteSlice(middle);
}

bool vtkWindowCube::goToSliceCoordinate(double typed)
{
    if (!m_simulationVolume)
        return false;
    const int axis = static_cast<int>(m_sliceAxis);
    // The file's coordinates are in code units; the number typed is in the
    // units shown (this axis's entry of the axis-unit table).
    const double code = typed / m_axisUnits.factor[std::size_t(axis)];
    bool inside = false;
    const int index = visivo::simvol::nearestPlane(code, this->remoteDatasetCrval[axis],
                                                   this->remoteDatasetCrpix[axis],
                                                   this->remoteDatasetCdelt[axis],
                                                   this->remoteSliceCount(), &inside);
    if (index < 0) {
        this->statusBar()->showMessage(u"This axis has no spacing: pick a plane with the slider."_s,
                                       5000);
        return false;
    }
    if (!inside) {
        const double a = this->remoteVoxelToWcs(axis, 0.0);
        const double b = this->remoteVoxelToWcs(axis, double(this->remoteSliceCount() - 1));
        this->statusBar()->showMessage(
                u"%1 is outside the grid (%2 to %3): showing the nearest plane."_s.arg(
                        visivo::units::formatNumber(typed), visivo::units::formatNumber(std::min(a, b)),
                        visivo::units::formatNumber(std::max(a, b))),
                6000);
    }
    ui->spinSlice->setValue(index + 1);   // through the debounce, as the slider does
    return inside;
}

void vtkWindowCube::syncSliceAxisUi()
{
    if (!m_simulationVolume)
        return;
    const QString letter = visivo::simvol::axisLetter(m_sliceAxis);
    for (ChannelOverlay *co : this->channelOverlays()) {
        co->setSimulationAxis(letter);
        co->setRange(1, this->remoteSliceCount());
    }
    const int shown = this->clampRemoteSliceIndex(ui->spinSlice->value() - 1);
    ui->labelSliceHdr->setText(u"CUTTING PLANE (%1)"_s.arg(letter.toUpper()));
    ui->labelSliceHdr->setToolTip(
            u"The slice is taken along %1: planes of constant %1, stepped with the slider or "
            u"at a typed coordinate (Inspector, Slice)."_s.arg(letter));
    ui->lineSpectral->setText(this->sliceCoordinateText(shown));
    for (ChannelOverlay *co : this->channelOverlays()) {
        co->setChannel(shown + 1);
        co->setSpectralLabel(ui->lineSpectral->text());
    }
    if (m_sliceAxisCombo) {
        const QSignalBlocker block(m_sliceAxisCombo);
        m_sliceAxisCombo->setCurrentIndex(static_cast<int>(m_sliceAxis));
    }
    if (m_sliceCoordEdit)
        m_sliceCoordEdit->setPlaceholderText(this->sliceCoordinateText(shown));
    this->invalidateWcsOverlayCache();
    this->requestWcsOverlayRender();
}

void vtkWindowCube::refreshSimulationRows()
{
    if (m_unitsControl)
        m_unitsControl->setUnits(m_physicalUnits, m_showPhysical);
    if (m_simTimeLabel) {
        const QString text = visivo::units::timeLabel(m_physicalUnits, m_showPhysical);
        m_simTimeLabel->setText(text.isEmpty() ? u"-"_s : text);
    }
}

void vtkWindowCube::setPhysicalDisplay(bool physical)
{
    if (!m_simulationVolume || physical == m_showPhysical)
        return;
    m_showPhysical = physical;
    this->refreshAxisUnits();
    // The value unit on the colour bars, probe and spectra axes.
    this->setIntensityUnit(m_codeBunit, m_intensityUnitNote);
    this->refreshSimulationRows();
    this->reloadForDisplayUnits();
}

void vtkWindowCube::reloadForDisplayUnits()
{
    if (this->remoteDatasetId.isEmpty())
        return;
    // Every data route now answers in the units asked for.
    BackendClient::setDisplayValueScale(
            this->remoteDatasetId,
            visivo::units::columnScale(m_physicalUnits, visivo::units::valueColumn(),
                                       m_showPhysical));
    this->remoteSliceCache.clear();
    this->hideSliceDetail();
    this->syncSliceAxisUi();
    this->requestRemoteSlice(this->clampRemoteSliceIndex(ui->spinSlice->value() - 1));
    // The 3-D cube again, from its preview, as the first open does. A preview
    // or full-resolution fetch still out was asked in the old units: its answer
    // is stale by id.
    ++this->currentRemoteHighResRequestId;
    this->usingHighResCube = false;
    this->m_deferredPreview.reset();
    this->setCubeOpenActionsEnabled(false);
    this->startRemotePreviewLoad();
    this->m_cubeStatsRequested = false;
    this->m_fullCubeStats = BackendCubeStatsResult{};
    this->fetchCubeStatsAsync();
}

void vtkWindowCube::addSimulationInspectorRows(InspectorPanel *panel)
{
    if (!panel || !m_simulationVolume)
        return;
    panel->addPropertyCaption(u"SLICE"_s);

    m_sliceAxisCombo = new QComboBox();
    m_sliceAxisCombo->addItems({ u"X"_s, u"Y"_s, u"Z"_s });
    m_sliceAxisCombo->setCurrentIndex(static_cast<int>(m_sliceAxis));
    m_sliceAxisCombo->setToolTip(u"The axis the slice plane is normal to: x, y or z."_s);
    QObject::connect(m_sliceAxisCombo, &QComboBox::currentIndexChanged, this, [this](int i) {
        this->setSliceAxis(visivo::simvol::axisFromIndex(i));
    });
    panel->addPropertyRow(u"Along"_s, m_sliceAxisCombo.data());

    m_sliceCoordEdit = new QLineEdit();
    auto *validator = new QDoubleValidator(m_sliceCoordEdit);
    validator->setNotation(QDoubleValidator::ScientificNotation);
    m_sliceCoordEdit->setValidator(validator);
    m_sliceCoordEdit->setToolTip(
            u"Type a coordinate in the units shown and press Enter: the slice goes to the "
            u"nearest plane."_s);
    QObject::connect(m_sliceCoordEdit, &QLineEdit::returnPressed, this, [this]() {
        bool ok = false;
        const double typed = m_sliceCoordEdit->text().toDouble(&ok);
        if (ok)
            this->goToSliceCoordinate(typed);
    });
    panel->addPropertyRow(u"At"_s, m_sliceCoordEdit.data());

    panel->addPropertyCaption(u"UNITS"_s);
    m_unitsControl = new PhysicalUnitsControl();
    m_unitsControl->setDataset(this->remoteBackendUrl, this->remoteBackendToken,
                               this->remoteSessionId, this->remoteDatasetId);
    QObject::connect(m_unitsControl, &PhysicalUnitsControl::displayChanged, this,
                     [this](bool physical) { this->setPhysicalDisplay(physical); });
    QObject::connect(m_unitsControl, &PhysicalUnitsControl::unitsChanged, this,
                     [this](const visivo::units::PhysicalUnits &units) {
                         // Units typed for a dataset that had none: shown at once.
                         if (!(m_physicalUnits.hasValue() || m_physicalUnits.hasLength()))
                             m_showPhysical = units.hasValue() || units.hasLength();
                         m_physicalUnits = units;
                         this->refreshAxisUnits();
                         this->setIntensityUnit(m_codeBunit, m_intensityUnitNote);
                         this->refreshSimulationRows();
                         this->reloadForDisplayUnits();
                     });
    panel->addPropertyRow(u"Display"_s, m_unitsControl.data());
    m_simTimeLabel = new QLabel(u"-"_s);
    panel->addPropertyRow(u"Time"_s, m_simTimeLabel.data());
    this->refreshSimulationRows();
}
