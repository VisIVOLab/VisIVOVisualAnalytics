#include "CubeUiAssembler.h"
#include "ui_vtkWindowCube.h"
#include "ColorMaps.h"

#include <vtkLookupTable.h>
#include <vtkNew.h>

#include <QAction>
#include <QColor>
#include <QComboBox>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QPixmap>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QToolButton>
#include <QVBoxLayout>
#include <QFormLayout>
#include <QWidget>

using namespace Qt::Literals::StringLiterals;

// ─────────────────────────────────────────────────────────────────────────────
// Sidebar style constants shared across all build methods
// ─────────────────────────────────────────────────────────────────────────────

static const QString kSidebarHdrStyle =
        u"QLabel {"
        "  color: #8AB8CC;"
        "  font-size: 10px;"
        "  font-weight: 600;"
        "  padding-bottom: 4px;"
        "  border-bottom: 1px solid rgba(4, 138, 191, 0.20);"
        "}"_s;

static const QString kSidebarCardStyle =
        u"QFrame {"
        "  background-color: rgba(255, 255, 255, 0.03);"
        "  border: 1px solid rgba(4, 138, 191, 0.14);"
        "  border-radius: 6px;"
        "}"_s;

static const QString kInfoHdrStyle =
        u"QLabel { font-size: 8px; font-weight: 700; letter-spacing: 1px;"
        "  color: rgba(4,138,191,0.75); margin-top: 6px; margin-bottom: 2px; }"_s;

static const QString kInfoCardStyle =
        u"QFrame {"
        "  background: rgba(255,255,255,0.04);"
        "  border: 1px solid rgba(4,138,191,0.18);"
        "  border-radius: 4px;"
        "}"_s;

static const QString kInfoRoStyle =
        u"QLineEdit[readOnly='true'] { color: #7AAABB; }"_s;

// ─────────────────────────────────────────────────────────────────────────────
// Construction
// ─────────────────────────────────────────────────────────────────────────────

CubeUiAssembler::CubeUiAssembler(Ui::vtkWindowCube *ui)
    : m_ui(ui)
{
}

CubeUiAssembler::~CubeUiAssembler() = default;

// ─────────────────────────────────────────────────────────────────────────────
// Static helpers
// ─────────────────────────────────────────────────────────────────────────────

QPixmap CubeUiAssembler::buildLutPreview(const QString &name, int w, int h)
{
    vtkNew<vtkLookupTable> lut;
    lut->SetTableRange(0., 1.);
    lut->SetNumberOfTableValues(w);
    ColorMaps::SetColorMap(lut, name.toStdString());
    lut->Build();

    QImage img(w, h, QImage::Format_RGB888);
    for (int x = 0; x < w; ++x) {
        const double t = (w > 1) ? static_cast<double>(x) / static_cast<double>(w - 1) : 0.0;
        double rgb[3];
        lut->GetColor(t, rgb);
        const QColor c(std::clamp(static_cast<int>(rgb[0] * 255.0 + 0.5), 0, 255),
                       std::clamp(static_cast<int>(rgb[1] * 255.0 + 0.5), 0, 255),
                       std::clamp(static_cast<int>(rgb[2] * 255.0 + 0.5), 0, 255));
        for (int y = 0; y < h; ++y)
            img.setPixel(x, y, c.rgb());
    }
    return QPixmap::fromImage(img);
}

// ─────────────────────────────────────────────────────────────────────────────
// buildThresholdPage  (was vtkWindowCube::setupThresholdPage)
// ─────────────────────────────────────────────────────────────────────────────

QWidget *CubeUiAssembler::buildThresholdPage()
{
    auto *page = new QWidget();
    auto *root = new QVBoxLayout(page);
    root->setContentsMargins(12, 14, 12, 12);
    root->setSpacing(12);

    // ── Section 1: Rendering Mode ─────────────────────────────────────────────
    {
        auto *hdr = new QLabel(u"RENDERING MODE"_s);
        hdr->setStyleSheet(kSidebarHdrStyle);
        root->addWidget(hdr);

        auto *card = new QFrame();
        card->setStyleSheet(kSidebarCardStyle);
        auto *cl = new QVBoxLayout(card);
        cl->setContentsMargins(8, 8, 8, 8);
        cl->setSpacing(0);

        m_comboRenderMode = new QComboBox();
        m_comboRenderMode->addItem(u"Isosurface"_s);
        m_comboRenderMode->addItem(u"Volume Rendering"_s);
        m_comboRenderMode->setItemData(0,
                u"Render an isosurface extracted at the rendering threshold."_s,
                Qt::ToolTipRole);
        m_comboRenderMode->setItemData(1,
                u"Render the cube volume directly (supports Composite, MIP, MinIP)."_s,
                Qt::ToolTipRole);
        m_comboRenderMode->setToolTip(u"Select how the 3D cube is rendered."_s);
        m_comboRenderMode->setCurrentIndex(m_ui->actionVolume->isChecked() ? 1 : 0);
        cl->addWidget(m_comboRenderMode);
        root->addWidget(card);
    }

    // ── Section 1b: Volume Mode (hidden when Isosurface is active) ───────────
    {
        auto *modeCard = new QFrame();
        modeCard->setStyleSheet(kSidebarCardStyle);
        auto *cl = new QVBoxLayout(modeCard);
        cl->setContentsMargins(8, 8, 8, 8);
        cl->setSpacing(4);

        auto *lbl = new QLabel(u"VOLUME RENDERING"_s);
        lbl->setStyleSheet(kSidebarHdrStyle);
        cl->addWidget(lbl);

        m_comboVolumeMode = new QComboBox();
        m_comboVolumeMode->addItem(u"Composite"_s);
        m_comboVolumeMode->addItem(u"MIP (Max Intensity)"_s);
        m_comboVolumeMode->addItem(u"MinIP (Min Intensity)"_s);
        m_comboVolumeMode->setToolTip(
                u"Choose the volume blending method.\n"
                "Composite: full ray-casting with opacity transfer function.\n"
                "MIP: maximum intensity projection along each ray.\n"
                "MinIP: minimum intensity projection along each ray."_s);
        m_comboVolumeMode->setCurrentIndex(0);
        cl->addWidget(m_comboVolumeMode);

        modeCard->setVisible(m_ui->actionVolume->isChecked());
        m_volumeModeCard = modeCard;
        root->addWidget(modeCard);
    }

    // ── Section 2: Rendering Threshold ───────────────────────────────────────
    {
        auto *hdr = new QLabel(u"RENDERING THRESHOLD"_s);
        hdr->setStyleSheet(kSidebarHdrStyle);
        root->addWidget(hdr);

        auto *card = new QFrame();
        card->setStyleSheet(kSidebarCardStyle);
        auto *cl = new QVBoxLayout(card);
        cl->setContentsMargins(8, 8, 8, 8);
        cl->setSpacing(6);

        m_ui->sliderThreshold->setParent(card);
        m_ui->sliderThreshold->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        m_ui->sliderThreshold->setToolTip(
                u"Set the rendering threshold for isosurface extraction and volume opacity."_s);
        cl->addWidget(m_ui->sliderThreshold);

        auto *row = new QHBoxLayout();
        row->setSpacing(6);
        m_ui->lineThreshold->setParent(card);
        m_ui->lineThreshold->setToolTip(u"Current rendering threshold value."_s);
        row->addWidget(m_ui->lineThreshold, 1);
        cl->addLayout(row);

        root->addWidget(card);
    }

    // ── Section 3: Surface Color (Isosurface mode only) ──────────────────────
    {
        auto *surfCard = new QFrame();
        surfCard->setStyleSheet(kSidebarCardStyle);
        auto *cl = new QVBoxLayout(surfCard);
        cl->setContentsMargins(8, 8, 8, 8);
        cl->setSpacing(6);

        auto *hdr = new QLabel(u"SURFACE COLOR"_s);
        hdr->setStyleSheet(kSidebarHdrStyle);
        cl->addWidget(hdr);

        auto *row = new QHBoxLayout();
        row->setSpacing(6);
        m_ui->btnCubeColor->setParent(surfCard);
        m_ui->btnCubeColor->setFixedSize(28, 28);
        m_ui->btnCubeColor->setToolTip(u"Select the surface color for isosurface rendering."_s);
        row->addWidget(m_ui->btnCubeColor);
        row->addStretch();
        cl->addLayout(row);

        surfCard->setVisible(!m_ui->actionVolume->isChecked());
        m_surfaceColorCard = surfCard;
        root->addWidget(surfCard);
    }

    // ── Section 4: Color Map (Volume mode only) ───────────────────────────────
    {
        auto *cmCard = new QFrame();
        cmCard->setStyleSheet(kSidebarCardStyle);
        auto *cl = new QVBoxLayout(cmCard);
        cl->setContentsMargins(8, 8, 8, 8);
        cl->setSpacing(6);

        auto *hdr = new QLabel(u"COLOR MAP"_s);
        hdr->setStyleSheet(kSidebarHdrStyle);
        cl->addWidget(hdr);

        m_comboLut = new QComboBox();
        m_comboLut->setIconSize(QSize(48, 12));
        const auto names = ColorMaps::GetColorMapNames();
        for (const auto &stdName : names) {
            const QString qName = QString::fromStdString(stdName);
            m_comboLut->addItem(QIcon(buildLutPreview(qName, 48, 12)), qName);
        }
        const QString defaultName = QString::fromStdString(ColorMaps::DefaultColorMap);
        m_comboLut->setCurrentText(defaultName);
        m_comboLut->setToolTip(u"Select the color map for volume rendering."_s);
        cl->addWidget(m_comboLut);

        cmCard->setVisible(m_ui->actionVolume->isChecked());
        m_colorMapCard = cmCard;
        root->addWidget(cmCard);
    }

    root->addStretch();
    return page;
}

// ─────────────────────────────────────────────────────────────────────────────
// build2dViewPage  (was vtkWindowCube::setup2dViewPage)
// ─────────────────────────────────────────────────────────────────────────────

QWidget *CubeUiAssembler::build2dViewPage()
{
    auto *page = new QWidget();
    auto *root = new QVBoxLayout(page);
    root->setContentsMargins(12, 14, 12, 12);
    root->setSpacing(12);

    const bool initiallySlice = m_ui->actionSlice->isChecked();

    // ── Section 1: 2D Mode ────────────────────────────────────────────────────
    {
        auto *hdr = new QLabel(u"2D MODE"_s);
        hdr->setStyleSheet(kSidebarHdrStyle);
        root->addWidget(hdr);

        auto *card = new QFrame();
        card->setStyleSheet(kSidebarCardStyle);
        auto *cl = new QVBoxLayout(card);
        cl->setContentsMargins(8, 8, 8, 8);
        cl->setSpacing(0);

        m_combo2dMode = new QComboBox();
        m_combo2dMode->addItem(u"Slice"_s);
        m_combo2dMode->addItem(u"Moment Map"_s);
        m_combo2dMode->setItemData(0, u"Display a 2D slice through the cube."_s, Qt::ToolTipRole);
        m_combo2dMode->setItemData(1, u"Display the computed moment map of the cube."_s, Qt::ToolTipRole);
        m_combo2dMode->setToolTip(u"Select the 2D display mode."_s);
        m_combo2dMode->setCurrentIndex(initiallySlice ? 0 : 1);
        cl->addWidget(m_combo2dMode);
        root->addWidget(card);
    }

    // ── Section 2: Slice Controls (Slice mode only) ───────────────────────────
    {
        auto *sliceCard = new QFrame();
        sliceCard->setStyleSheet(kSidebarCardStyle);
        auto *cl = new QVBoxLayout(sliceCard);
        cl->setContentsMargins(8, 8, 8, 8);
        cl->setSpacing(6);

        auto *hdr = new QLabel(u"CUTTING PLANE"_s);
        hdr->setStyleSheet(kSidebarHdrStyle);
        cl->addWidget(hdr);

        m_ui->sliderSlice->setParent(sliceCard);
        m_ui->sliderSlice->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        cl->addWidget(m_ui->sliderSlice);

        auto *row = new QHBoxLayout();
        row->setSpacing(6);
        m_ui->spinSlice->setParent(sliceCard);
        row->addWidget(m_ui->spinSlice);
        m_ui->lineSpectral->setParent(sliceCard);
        row->addWidget(m_ui->lineSpectral, 1);
        cl->addLayout(row);

        sliceCard->setVisible(initiallySlice);
        m_sliceControlsCard = sliceCard;
        root->addWidget(sliceCard);
    }

    // ── Section 3: Slice Color Map (Slice mode only) ──────────────────────────
    {
        auto *lutCard = new QFrame();
        lutCard->setStyleSheet(kSidebarCardStyle);
        auto *cl = new QVBoxLayout(lutCard);
        cl->setContentsMargins(8, 8, 8, 8);
        cl->setSpacing(6);

        auto *hdr = new QLabel(u"COLOR MAP"_s);
        hdr->setStyleSheet(kSidebarHdrStyle);
        cl->addWidget(hdr);

        m_comboSliceLut = new QComboBox();
        m_comboSliceLut->setIconSize(QSize(48, 12));
        const auto names = ColorMaps::GetColorMapNames();
        for (const auto &stdName : names) {
            const QString qName = QString::fromStdString(stdName);
            m_comboSliceLut->addItem(QIcon(buildLutPreview(qName, 48, 12)), qName);
        }
        const QString defaultName = QString::fromStdString(ColorMaps::DefaultColorMap);
        m_comboSliceLut->setCurrentText(defaultName);
        m_comboSliceLut->setToolTip(u"Select the color map for slice views."_s);
        cl->addWidget(m_comboSliceLut);

        lutCard->setVisible(initiallySlice);
        m_sliceLutCard = lutCard;
        root->addWidget(lutCard);
    }

    // ── Section 4: Contours (Slice mode only) ─────────────────────────────────
    {
        auto *hdr = new QLabel(u"CONTOURS"_s);
        hdr->setStyleSheet(kSidebarHdrStyle);

        m_ui->frameContours->setStyleSheet(kSidebarCardStyle);
        m_ui->frameContours->setParent(page);

        auto *contoursCard = new QWidget();
        auto *ccl = new QVBoxLayout(contoursCard);
        ccl->setContentsMargins(0, 0, 0, 0);
        ccl->setSpacing(4);
        ccl->addWidget(hdr);
        ccl->addWidget(m_ui->frameContours);

        contoursCard->setVisible(initiallySlice);
        m_sliceContoursCard = contoursCard;
        root->addWidget(contoursCard);
    }

    // ── Section 5: Moment Map Info (Moment mode only) ─────────────────────────
    {
        auto *momentCard = new QFrame();
        momentCard->setStyleSheet(kSidebarCardStyle);
        auto *cl = new QVBoxLayout(momentCard);
        cl->setContentsMargins(8, 8, 8, 8);
        cl->setSpacing(4);

        auto *hdr = new QLabel(u"MOMENT MAP"_s);
        hdr->setStyleSheet(kSidebarHdrStyle);
        cl->addWidget(hdr);

        auto *info = new QLabel(
                u"No moment map available.\nOpen Compute \u2192 Moment Map\nto generate one."_s);
        info->setWordWrap(true);
        info->setStyleSheet(u"QLabel { color: #7AAABB; font-size: 9px; }"_s);
        cl->addWidget(info);
        m_momentInfoLabel = info;

        momentCard->setVisible(!initiallySlice);
        m_momentInfoCard = momentCard;
        root->addWidget(momentCard);
    }

    // ── Section 6: Moment Color Map (Moment mode only) ────────────────────────
    {
        auto *lutCard = new QFrame();
        lutCard->setStyleSheet(kSidebarCardStyle);
        auto *cl = new QVBoxLayout(lutCard);
        cl->setContentsMargins(8, 8, 8, 8);
        cl->setSpacing(6);

        auto *hdr = new QLabel(u"COLOR MAP"_s);
        hdr->setStyleSheet(kSidebarHdrStyle);
        cl->addWidget(hdr);

        m_comboMomentLut = new QComboBox();
        m_comboMomentLut->setIconSize(QSize(48, 12));
        const auto names2 = ColorMaps::GetColorMapNames();
        for (const auto &stdName : names2) {
            const QString qName = QString::fromStdString(stdName);
            m_comboMomentLut->addItem(QIcon(buildLutPreview(qName, 48, 12)), qName);
        }
        const QString defaultName2 = QString::fromStdString(ColorMaps::DefaultColorMap);
        m_comboMomentLut->setCurrentText(defaultName2);
        m_comboMomentLut->setToolTip(u"Select the color map for moment map views."_s);
        cl->addWidget(m_comboMomentLut);

        lutCard->setVisible(!initiallySlice);
        m_momentLutCard = lutCard;
        root->addWidget(lutCard);
    }

    root->addStretch();
    return page;
}

// ─────────────────────────────────────────────────────────────────────────────
// buildToolsPage  (was vtkWindowCube::setupToolsPage)
// ─────────────────────────────────────────────────────────────────────────────

QWidget *CubeUiAssembler::buildToolsPage()
{
    auto *page = new QWidget();
    auto *root = new QVBoxLayout(page);
    root->setContentsMargins(12, 14, 12, 12);
    root->setSpacing(12);

    // Helper: create a placeholder QToolButton.
    // Text and enabled state are overwritten by wireToolActions() once the
    // QActions exist (they are created after setupSidebar() returns).
    auto makeToolBtn = [](const QString &label) -> QToolButton * {
        auto *btn = new QToolButton();
        btn->setText(label);
        btn->setEnabled(false);
        btn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        btn->setToolButtonStyle(Qt::ToolButtonTextOnly);
        return btn;
    };

    // ── ANALYSIS ──────────────────────────────────────────────────────────────
    {
        auto *hdr = new QLabel(u"ANALYSIS"_s);
        hdr->setStyleSheet(kSidebarHdrStyle);
        root->addWidget(hdr);

        auto *card = new QFrame();
        card->setStyleSheet(kSidebarCardStyle);
        auto *cl = new QVBoxLayout(card);
        cl->setContentsMargins(8, 6, 8, 6);
        cl->setSpacing(4);

        m_btnEstimateNoise = makeToolBtn(u"Estimate Noise\u2026"_s);
        cl->addWidget(m_btnEstimateNoise);
        root->addWidget(card);
    }

    // ── EXPLORATION ───────────────────────────────────────────────────────────
    {
        auto *hdr = new QLabel(u"EXPLORATION"_s);
        hdr->setStyleSheet(kSidebarHdrStyle);
        root->addWidget(hdr);

        auto *card = new QFrame();
        card->setStyleSheet(kSidebarCardStyle);
        auto *cl = new QVBoxLayout(card);
        cl->setContentsMargins(8, 6, 8, 6);
        cl->setSpacing(4);

        m_btnDataProbe = makeToolBtn(u"Data Probe"_s);
        m_btnPvDiagram = makeToolBtn(u"PV Diagram"_s);
        cl->addWidget(m_btnDataProbe);
        cl->addWidget(m_btnPvDiagram);
        root->addWidget(card);
    }

    // ── VIEW ──────────────────────────────────────────────────────────────────
    {
        auto *hdr = new QLabel(u"VIEW"_s);
        hdr->setStyleSheet(kSidebarHdrStyle);
        root->addWidget(hdr);

        auto *card = new QFrame();
        card->setStyleSheet(kSidebarCardStyle);
        auto *cl = new QVBoxLayout(card);
        cl->setContentsMargins(8, 6, 8, 6);
        cl->setSpacing(4);

        m_btnCameraRoi = makeToolBtn(u"Use Camera ROI"_s);
        cl->addWidget(m_btnCameraRoi);
        root->addWidget(card);
    }

    root->addStretch();
    return page;
}

// ─────────────────────────────────────────────────────────────────────────────
// buildInfoStatsPage  (was vtkWindowCube::setupInfoStatsPage)
// ─────────────────────────────────────────────────────────────────────────────

QWidget *CubeUiAssembler::buildInfoStatsPage(const InfoStatsInput &info)
{
    auto makeVal = [](const QString &text = {}) -> QLabel * {
        auto *l = new QLabel(text);
        l->setWordWrap(true);
        l->setStyleSheet(u"QLabel { font-size: 9px; color: #AABBCC; }"_s);
        return l;
    };

    auto *page = new QWidget();
    auto *root = new QVBoxLayout(page);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(4);

    // ── 1. DATASET SUMMARY ────────────────────────────────────────────────────
    {
        auto *hdr = new QLabel(u"DATASET SUMMARY"_s);
        hdr->setStyleSheet(kInfoHdrStyle);
        root->addWidget(hdr);

        auto *card = new QFrame();
        card->setStyleSheet(kInfoCardStyle);
        auto *fl = new QFormLayout(card);
        fl->setContentsMargins(8, 6, 8, 6);
        fl->setSpacing(3);
        fl->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);

        m_infoDsName = makeVal(QFileInfo(info.filepath).fileName());
        m_infoDsDimensions = makeVal(
                u"%1 \u00d7 %2 \u00d7 %3"_s
                        .arg(info.datasetWidth)
                        .arg(info.datasetHeight)
                        .arg(info.datasetDepth));
        m_infoDsLoadState = makeVal(u"Preview"_s);

        fl->addRow(u"File:"_s,       m_infoDsName);
        fl->addRow(u"Dimensions:"_s, m_infoDsDimensions);
        fl->addRow(u"Load state:"_s, m_infoDsLoadState);
        root->addWidget(card);
    }

    // ── 2. DATA STATISTICS ────────────────────────────────────────────────────
    {
        auto *hdr = new QLabel(u"DATA STATISTICS"_s);
        hdr->setStyleSheet(kInfoHdrStyle);
        root->addWidget(hdr);

        auto *subHdr2d = new QLabel(u"2D IMAGE"_s);
        subHdr2d->setStyleSheet(
                u"QLabel { font-size: 8px; font-weight: 600; letter-spacing: 1px; color: rgba(4,138,191,0.55); }"_s);
        root->addWidget(subHdr2d);

        m_ui->labelImg->setStyleSheet(
                u"QLabel { font-size: 8px; font-weight: 600; letter-spacing: 1px; color: rgba(4,138,191,0.55); }"_s);
        m_ui->frameImgStats->setStyleSheet(kInfoCardStyle + kInfoRoStyle);
        m_ui->frameImgStats->setParent(page);
        root->addWidget(m_ui->labelImg);
        root->addWidget(m_ui->frameImgStats);

        auto *subHdr3d = new QLabel(u"3D CUBE"_s);
        subHdr3d->setStyleSheet(
                u"QLabel { font-size: 8px; font-weight: 600; letter-spacing: 1px; color: rgba(4,138,191,0.55); }"_s);
        root->addWidget(subHdr3d);

        m_ui->frameCubeStats->setStyleSheet(kInfoCardStyle + kInfoRoStyle);
        m_ui->frameCubeStats->setParent(page);
        root->addWidget(m_ui->frameCubeStats);
    }

    // ── 3. COORDINATE / WCS ───────────────────────────────────────────────────
    {
        auto *hdr = new QLabel(u"COORDINATE / WCS"_s);
        hdr->setStyleSheet(kInfoHdrStyle);
        root->addWidget(hdr);

        auto *card = new QFrame();
        card->setStyleSheet(kInfoCardStyle);
        auto *fl = new QFormLayout(card);
        fl->setContentsMargins(8, 6, 8, 6);
        fl->setSpacing(3);
        fl->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);

        m_infoWcsBadge = makeVal();
        m_infoWcsFrame = makeVal(info.wcsFrameLabel);
        m_infoSpectral = makeVal(info.spectralAxisTitle);

        m_infoWcsBadge->setToolTip(u"World Coordinate System status for this dataset."_s);
        m_infoWcsFrame->setToolTip(u"The celestial reference frame currently in use."_s);
        m_infoSpectral->setToolTip(u"The spectral axis type and units for the third dimension."_s);

        // Populate WCS badge with initial status.
        if (info.wcsStatus.compare(u"ok"_s, Qt::CaseInsensitive) == 0) {
            m_infoWcsBadge->setText(u"OK"_s);
            m_infoWcsBadge->setStyleSheet(u"QLabel { font-size: 9px; color: #5DA87A; }"_s);
        } else if (info.wcsStatus.compare(u"degraded"_s, Qt::CaseInsensitive) == 0) {
            m_infoWcsBadge->setText(u"Degraded"_s);
            m_infoWcsBadge->setStyleSheet(u"QLabel { font-size: 9px; color: #E8A840; }"_s);
        } else {
            m_infoWcsBadge->setText(u"Repaired"_s);
            m_infoWcsBadge->setStyleSheet(u"QLabel { font-size: 9px; color: #9a6700; }"_s);
        }
        if (!info.wcsWarningMessage.isEmpty())
            m_infoWcsBadge->setToolTip(info.wcsWarningMessage);

        fl->addRow(u"WCS:"_s,          m_infoWcsBadge);
        fl->addRow(u"Frame:"_s,        m_infoWcsFrame);
        fl->addRow(u"Spectral axis:"_s, m_infoSpectral);
        root->addWidget(card);
    }

    // ── 4. SYSTEM / STATUS ────────────────────────────────────────────────────
    {
        auto *hdr = new QLabel(u"SYSTEM / STATUS"_s);
        hdr->setStyleSheet(kInfoHdrStyle);
        root->addWidget(hdr);

        auto *card = new QFrame();
        card->setStyleSheet(kInfoCardStyle);
        auto *fl = new QFormLayout(card);
        fl->setContentsMargins(8, 6, 8, 6);
        fl->setSpacing(3);
        fl->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);

        m_infoRenderMode = makeVal(u"Isosurface"_s);
        m_info2dMode     = makeVal(u"Slice"_s);
        m_infoMomentState = makeVal(u"\u2014"_s);

        m_infoRenderMode->setToolTip(u"Current 3D rendering mode (Isosurface or Volume Rendering)."_s);
        m_info2dMode->setToolTip(u"Current 2D view mode (Slice or Moment Map)."_s);
        m_infoMomentState->setToolTip(u"State of the last moment map computation."_s);

        fl->addRow(u"Rendering mode:"_s, m_infoRenderMode);
        fl->addRow(u"2D view:"_s,        m_info2dMode);
        fl->addRow(u"Moment map:"_s,     m_infoMomentState);
        root->addWidget(card);
    }

    root->addStretch();
    return page;
}

// ─────────────────────────────────────────────────────────────────────────────
// wireToolActions  (new – called after QActions are constructed)
// ─────────────────────────────────────────────────────────────────────────────

void CubeUiAssembler::wireToolActions(QAction *estimateNoise,
                                      QAction *extractSpectrum,
                                      QAction *extractPvDiagram,
                                      QAction *cameraRoi)
{
    // setDefaultAction() links button text, tooltip, enabled state, and
    // checkability to the action — keeps the sidebar and menu in sync.
    if (m_btnEstimateNoise && estimateNoise)
        m_btnEstimateNoise->setDefaultAction(estimateNoise);

    if (m_btnDataProbe && extractSpectrum) {
        m_btnDataProbe->setDefaultAction(extractSpectrum);
        m_btnDataProbe->setToolTip(u"Probe data values at a point in the 2D view."_s);
    }
    if (m_btnPvDiagram && extractPvDiagram) {
        m_btnPvDiagram->setDefaultAction(extractPvDiagram);
        m_btnPvDiagram->setToolTip(
                u"Draw a path in the 2D view to extract a position-velocity diagram."_s);
    }
    if (m_btnCameraRoi && cameraRoi)
        m_btnCameraRoi->setDefaultAction(cameraRoi);
}

// ─────────────────────────────────────────────────────────────────────────────
// Sync helpers  (were vtkWindowCube::sync2dModeSidebar / syncVolumeModeSidebar)
// ─────────────────────────────────────────────────────────────────────────────

void CubeUiAssembler::sync2dModeSidebar(bool sliceActive)
{
    if (!m_combo2dMode)
        return;
    const QSignalBlocker b(m_combo2dMode);
    m_combo2dMode->setCurrentIndex(sliceActive ? 0 : 1);
}

void CubeUiAssembler::syncVolumeModeSidebar(int idx)
{
    if (!m_comboVolumeMode)
        return;
    const QSignalBlocker b(m_comboVolumeMode);
    m_comboVolumeMode->setCurrentIndex(idx);
}

// ─────────────────────────────────────────────────────────────────────────────
// updateMomentCard  — pure label rendering from pre-built MomentCardData
// ─────────────────────────────────────────────────────────────────────────────

void CubeUiAssembler::updateMomentCard(const MomentCardData &data)
{
    auto *lbl = m_momentInfoLabel.data();
    if (!lbl)
        return;

    static const QString infoColor  = u"color: #7AAABB;"_s;
    static const QString busyColor  = u"color: #E8A840;"_s;
    static const QString errorColor = u"color: #CC6060;"_s;

    switch (data.state) {
    case MomentCardData::State::NoMoment:
        lbl->setStyleSheet(QStringLiteral("QLabel { font-size: 9px; %1 }").arg(infoColor));
        lbl->setText(u"No moment map available.\nOpen Compute \u2192 Moment Map\nto generate one."_s);
        lbl->setToolTip(QString());
        break;
    case MomentCardData::State::Computing:
        lbl->setStyleSheet(QStringLiteral("QLabel { font-size: 9px; font-style: italic; %1 }").arg(busyColor));
        lbl->setText(u"Computing moment map\u2026\n%1"_s.arg(data.detail));
        lbl->setToolTip(u"Moment computation is running on the backend.\nThe panel will update automatically when done."_s);
        break;
    case MomentCardData::State::Ready:
        lbl->setStyleSheet(QStringLiteral("QLabel { font-size: 9px; %1 }").arg(infoColor));
        lbl->setText(data.provenanceSummary);
        lbl->setToolTip(data.provenanceDetails);
        break;
    case MomentCardData::State::Error:
        lbl->setStyleSheet(QStringLiteral("QLabel { font-size: 9px; %1 }").arg(errorColor));
        lbl->setText(u"Moment computation failed.\n%1"_s.arg(
                data.detail.isEmpty()
                        ? u"Check the Diagnostics panel for details."_s
                        : data.detail));
        lbl->setToolTip(u"The last moment computation did not produce a result.\nSee the Diagnostics panel for the error."_s);
        break;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// updateInfoStats  — pure label rendering from pre-built InfoStatsData
// ─────────────────────────────────────────────────────────────────────────────

void CubeUiAssembler::updateInfoStats(const InfoStatsData &data)
{
    if (auto *l = m_infoDsLoadState.data()) {
        l->setText(data.loadStateText);
        l->setStyleSheet(data.loadStateStyleSheet);
    }
    if (auto *l = m_infoWcsFrame.data())
        l->setText(data.wcsFrame);
    if (auto *l = m_infoSpectral.data())
        l->setText(data.spectralAxis);
    if (auto *badge = m_infoWcsBadge.data()) {
        badge->setText(data.wcsBadgeText);
        badge->setStyleSheet(data.wcsBadgeStyleSheet);
        if (!data.wcsBadgeTooltip.isEmpty())
            badge->setToolTip(data.wcsBadgeTooltip);
    }
    if (auto *l = m_infoRenderMode.data())
        l->setText(data.renderModeText);
    if (auto *l = m_info2dMode.data())
        l->setText(data.imageModeText);
    if (auto *l = m_infoMomentState.data()) {
        l->setText(data.momentStateText);
        l->setStyleSheet(data.momentStateStyleSheet);
    }
}
