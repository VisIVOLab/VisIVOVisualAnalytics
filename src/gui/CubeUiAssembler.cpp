#include "CubeUiAssembler.h"
#include "ui_vtkWindowCube.h"
#include "ColorMaps.h"
#include "theme/VisivoTheme.h"
#include "theme/SegmentedToggle.h"

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
#include <QStatusBar>
#include <QToolButton>
#include <QVBoxLayout>
#include <QFormLayout>
#include <QWidget>

using namespace Qt::Literals::StringLiterals;

// ─────────────────────────────────────────────────────────────────────────────
// Sidebar style helpers — built lazily so they pick up the theme that was
// applied AFTER static initialization (ThemeManager runs in main()).
// Returning fresh QStrings keeps the logic theme-aware on every call.
// ─────────────────────────────────────────────────────────────────────────────

static QString kSidebarHdrStyle()
{
    return QStringLiteral(
                   "QLabel {"
                   "  color: %1;"
                   "  font-size: 10px;"
                   "  font-weight: 600;"
                   "  padding-bottom: 4px;"
                   "  border-bottom: 1px solid rgba(4, 138, 191, 0.20);"
                   "}")
            .arg(QLatin1StringView(VisivoTheme::kOnSurfaceVariant()));
}

static QString kSidebarCardStyle()
{
    // Idle card tint: subtle white-on-dark or black-on-light overlay so the
    // card is distinguishable from the surrounding panel without dominating.
    const QLatin1StringView tint = VisivoTheme::isDark()
            ? "rgba(255, 255, 255, 0.03)"_L1
            : "rgba(0, 0, 0, 0.03)"_L1;
    return QStringLiteral(
                   "QFrame {"
                   "  background-color: %1;"
                   "  border: 1px solid rgba(4, 138, 191, 0.14);"
                   "  border-radius: 6px;"
                   "}")
            .arg(tint);
}

static QString kInfoHdrStyle()
{
    // Brand-blue overlay reads on either theme — leave fixed.
    return QStringLiteral(
            "QLabel { font-size: 8px; font-weight: 700; letter-spacing: 1px;"
            "  color: rgba(4,138,191,0.75); margin-top: 6px; margin-bottom: 2px; }");
}

/// Shared helper: create a disabled placeholder QToolButton for the sidebar.
/// Text and enabled state are overwritten by wireToolActions() once the
/// corresponding QAction exists.
static QToolButton *makeSidebarToolButton(const QString &label)
{
    auto *btn = new QToolButton();
    btn->setText(label);
    btn->setEnabled(false);
    btn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    btn->setToolButtonStyle(Qt::ToolButtonTextOnly);
    return btn;
}

static const QString kInfoCardStyle =
        u"QFrame {"
        u"  background: rgba(255,255,255,0.04);"
        u"  border: 1px solid rgba(4,138,191,0.18);"
        u"  border-radius: 4px;"
        u"}"_s;

// ── Inspector (sidebar "Inspector" page) — visual tokens matching the
// ── Observatory Console design: flat sections separated by 1px dividers,
// ── KV rows with muted label / monospace value, optional file badge header.
// ──
// ── See docs/architecture-note.md "Cube viewer — interactive extras" for the
// ── higher-level rationale.

/// Section divider: a `QFrame` with a single bottom hairline border.
static QString kInspectorSectionStyle()
{
    return QStringLiteral(
                   "QFrame#inspectorSection {"
                   "  background: transparent;"
                   "  border: none;"
                   "  border-bottom: 1px solid %1;"
                   "}")
            .arg(QLatin1StringView(VisivoTheme::kOutline()));
}

/// Uppercase section title (DATASET, STATISTICS, …)
static QString kInspectorTitleStyle()
{
    return QStringLiteral(
                   "QLabel {"
                   "  color: %1;"
                   "  font-size: 11px;"
                   "  font-weight: 700;"
                   "  letter-spacing: 0.6px;"
                   "  text-transform: uppercase;"
                   "  padding: 0;"
                   "  border: none;"
                   "}")
            .arg(QLatin1StringView(VisivoTheme::kOnSurface()));
}

/// Sub-section header inside a section (e.g. "2D IMAGE", "3D CUBE", "WCS · J2000").
static QString kInspectorSubLabelStyle()
{
    return QStringLiteral(
                   "QLabel {"
                   "  color: %1;"
                   "  font-size: 10px;"
                   "  font-weight: 600;"
                   "  letter-spacing: 0.4px;"
                   "  margin-top: 6px;"
                   "  margin-bottom: 2px;"
                   "}")
            .arg(QLatin1StringView(VisivoTheme::kSecondary()));
}

/// KV row label (left, secondary text).
static QString kInspectorKvLabelStyle()
{
    return QStringLiteral("QLabel { color: %1; font-size: 11px; border: none; }")
            .arg(QLatin1StringView(VisivoTheme::kSecondary()));
}

/// KV row value (right, monospace, primary text).
static QString kInspectorKvValueStyle()
{
    return QStringLiteral(
                   "QLabel { color: %1; font-family: Menlo, Consolas, monospace;"
                   "  font-size: 12px; border: none; }")
            .arg(QLatin1StringView(VisivoTheme::kOnSurface()));
}

/// Small inline pill badge — used in the inspector file header.
static QString kInspectorBadgeStyle()
{
    return QStringLiteral(
            "QLabel {"
            "  color: #048ABF;"
            "  background: rgba(4, 138, 191, 0.18);"
            "  font-family: Menlo, Consolas, monospace;"
            "  font-size: 9px;"
            "  font-weight: 700;"
            "  letter-spacing: 0.6px;"
            "  padding: 1px 5px;"
            "  border: none;"
            "  border-radius: 3px;"
            "}");
}

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

// Moved from class scope: only used in this translation unit.
static QPixmap buildLutPreview(const QString &name, int w, int h)
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
        hdr->setStyleSheet(kSidebarHdrStyle());
        root->addWidget(hdr);

        auto *card = new QFrame();
        card->setStyleSheet(kSidebarCardStyle());
        auto *cl = new QVBoxLayout(card);
        cl->setContentsMargins(8, 8, 8, 8);
        cl->setSpacing(0);

        m_comboRenderMode = new SegmentedToggle();
        m_comboRenderMode->addItem(
                u"Isosurface"_s,
                u"Render an isosurface extracted at the rendering threshold."_s);
        m_comboRenderMode->addItem(
                u"Volume"_s,
                u"Render the cube volume directly (supports Composite, MIP, MinIP)."_s);
        m_comboRenderMode->setToolTip(u"Select how the 3D cube is rendered."_s);
        m_comboRenderMode->setCurrentIndex(m_ui->actionVolume->isChecked() ? 1 : 0);
        cl->addWidget(m_comboRenderMode);
        root->addWidget(card);
    }

    // ── Section 1b: Volume Mode (hidden when Isosurface is active) ───────────
    {
        auto *modeCard = new QFrame();
        modeCard->setStyleSheet(kSidebarCardStyle());
        auto *cl = new QVBoxLayout(modeCard);
        cl->setContentsMargins(8, 8, 8, 8);
        cl->setSpacing(4);

        auto *lbl = new QLabel(u"VOLUME RENDERING"_s);
        lbl->setStyleSheet(kSidebarHdrStyle());
        cl->addWidget(lbl);

        m_comboVolumeMode = new SegmentedToggle();
        m_comboVolumeMode->addItem(
                u"Composite"_s,
                u"Full ray-casting with opacity transfer function."_s);
        m_comboVolumeMode->addItem(
                u"MIP"_s,
                u"Maximum intensity projection along each ray."_s);
        m_comboVolumeMode->addItem(
                u"MinIP"_s,
                u"Minimum intensity projection along each ray."_s);
        m_comboVolumeMode->setToolTip(
                u"Choose the volume blending method."_s);
        m_comboVolumeMode->setCurrentIndex(0);
        cl->addWidget(m_comboVolumeMode);

        modeCard->setVisible(m_ui->actionVolume->isChecked());
        m_volumeModeCard = modeCard;
        root->addWidget(modeCard);
    }

    // ── Section 2: Rendering Threshold ───────────────────────────────────────
    {
        auto *hdr = new QLabel(u"RENDERING THRESHOLD"_s);
        hdr->setStyleSheet(kSidebarHdrStyle());
        root->addWidget(hdr);

        auto *card = new QFrame();
        card->setStyleSheet(kSidebarCardStyle());
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
        surfCard->setStyleSheet(kSidebarCardStyle());
        auto *cl = new QVBoxLayout(surfCard);
        cl->setContentsMargins(8, 8, 8, 8);
        cl->setSpacing(6);

        auto *hdr = new QLabel(u"SURFACE COLOR"_s);
        hdr->setStyleSheet(kSidebarHdrStyle());
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
        cmCard->setStyleSheet(kSidebarCardStyle());
        auto *cl = new QVBoxLayout(cmCard);
        cl->setContentsMargins(8, 8, 8, 8);
        cl->setSpacing(6);

        auto *hdr = new QLabel(u"COLOR MAP"_s);
        hdr->setStyleSheet(kSidebarHdrStyle());
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

        m_btnLutEdit3D = new QToolButton();
        m_btnLutEdit3D->setText(u"Advanced\u2026"_s);
        m_btnLutEdit3D->setToolTip(u"Open advanced LUT editor for 3D volume rendering"_s);
        m_btnLutEdit3D->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        m_btnLutEdit3D->setToolButtonStyle(Qt::ToolButtonTextOnly);
        cl->addWidget(m_btnLutEdit3D);

        cmCard->setVisible(m_ui->actionVolume->isChecked());
        m_colorMapCard = cmCard;
        root->addWidget(cmCard);
    }

    // ── Camera ROI ────────────────────────────────────────────────────────────
    // Placed here because it controls how the 3D renderer refines the view.
    {
        auto *hdr = new QLabel(u"RENDERING OPTIONS"_s);
        hdr->setStyleSheet(kSidebarHdrStyle());
        root->addWidget(hdr);

        auto *card = new QFrame();
        card->setStyleSheet(kSidebarCardStyle());
        auto *cl = new QVBoxLayout(card);
        cl->setContentsMargins(8, 6, 8, 6);
        cl->setSpacing(4);

        m_btnCameraRoi = makeSidebarToolButton(u"Use Camera ROI"_s);
        cl->addWidget(m_btnCameraRoi);
        root->addWidget(card);
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
        hdr->setStyleSheet(kSidebarHdrStyle());
        root->addWidget(hdr);

        auto *card = new QFrame();
        card->setStyleSheet(kSidebarCardStyle());
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
        sliceCard->setStyleSheet(kSidebarCardStyle());
        auto *cl = new QVBoxLayout(sliceCard);
        cl->setContentsMargins(8, 8, 8, 8);
        cl->setSpacing(6);

        auto *hdr = new QLabel(u"CUTTING PLANE"_s);
        hdr->setStyleSheet(kSidebarHdrStyle());
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
        lutCard->setStyleSheet(kSidebarCardStyle());
        auto *cl = new QVBoxLayout(lutCard);
        cl->setContentsMargins(8, 8, 8, 8);
        cl->setSpacing(6);

        auto *hdr = new QLabel(u"COLOR MAP"_s);
        hdr->setStyleSheet(kSidebarHdrStyle());
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

        m_btnLutEditSlice = new QToolButton();
        m_btnLutEditSlice->setText(u"Advanced\u2026"_s);
        m_btnLutEditSlice->setToolTip(u"Open advanced LUT editor for the 2D slice"_s);
        m_btnLutEditSlice->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        m_btnLutEditSlice->setToolButtonStyle(Qt::ToolButtonTextOnly);
        cl->addWidget(m_btnLutEditSlice);

        lutCard->setVisible(initiallySlice);
        m_sliceLutCard = lutCard;
        root->addWidget(lutCard);
    }

    // ── Section 4: Contours (Slice mode only) ─────────────────────────────────
    {
        auto *hdr = new QLabel(u"CONTOURS"_s);
        hdr->setStyleSheet(kSidebarHdrStyle());

        m_ui->frameContours->setStyleSheet(kSidebarCardStyle());
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
        momentCard->setStyleSheet(kSidebarCardStyle());
        auto *cl = new QVBoxLayout(momentCard);
        cl->setContentsMargins(8, 8, 8, 8);
        cl->setSpacing(4);

        auto *hdr = new QLabel(u"MOMENT MAP"_s);
        hdr->setStyleSheet(kSidebarHdrStyle());
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
        lutCard->setStyleSheet(kSidebarCardStyle());
        auto *cl = new QVBoxLayout(lutCard);
        cl->setContentsMargins(8, 8, 8, 8);
        cl->setSpacing(6);

        auto *hdr = new QLabel(u"COLOR MAP"_s);
        hdr->setStyleSheet(kSidebarHdrStyle());
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

        m_btnLutEditMoment = new QToolButton();
        m_btnLutEditMoment->setText(u"Advanced\u2026"_s);
        m_btnLutEditMoment->setToolTip(u"Open advanced LUT editor for the moment map"_s);
        m_btnLutEditMoment->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        m_btnLutEditMoment->setToolButtonStyle(Qt::ToolButtonTextOnly);
        cl->addWidget(m_btnLutEditMoment);

        lutCard->setVisible(!initiallySlice);
        m_momentLutCard = lutCard;
        root->addWidget(lutCard);
    }

    // ── WCS Axes overlay ──────────────────────────────────────────────────────
    // Placed here because it is an overlay on the 2D view.
    {
        auto *hdr = new QLabel(u"OVERLAYS"_s);
        hdr->setStyleSheet(kSidebarHdrStyle());
        root->addWidget(hdr);

        auto *card = new QFrame();
        card->setStyleSheet(kSidebarCardStyle());
        auto *cl = new QVBoxLayout(card);
        cl->setContentsMargins(8, 6, 8, 6);
        cl->setSpacing(4);

        m_btnWcsAxes = makeSidebarToolButton(u"Show WCS Axes"_s);
        cl->addWidget(m_btnWcsAxes);
        root->addWidget(card);
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

    // Local helper: build one group (header + card + buttons), same visual
    // treatment as before but factored out so each of the three groups stays
    // one block long. Returns the card's inner layout, ready for buttons.
    auto makeGroup = [&](const QString &title) -> QVBoxLayout * {
        auto *hdr = new QLabel(title);
        hdr->setStyleSheet(kSidebarHdrStyle());
        root->addWidget(hdr);
        auto *card = new QFrame();
        card->setStyleSheet(kSidebarCardStyle());
        auto *cl = new QVBoxLayout(card);
        cl->setContentsMargins(8, 6, 8, 6);
        cl->setSpacing(4);
        root->addWidget(card);
        return cl;
    };

    // EXPLORATION group: interactive viewing -- point probe + VR offload.
    {
        auto *cl = makeGroup(u"EXPLORATION"_s);
        m_btnDataProbe = makeSidebarToolButton(u"Data Probe"_s);
        m_btnOpenInVR  = makeSidebarToolButton(u"Open in VR"_s);
        cl->addWidget(m_btnDataProbe);
        cl->addWidget(m_btnOpenInVR);
    }

    // ANALYSIS group: all compute-on-cube tools in the same order as the
    // Tools menu (noise -> moment -> line-width -> baseline -> stack -> PV).
    {
        auto *cl = makeGroup(u"ANALYSIS"_s);
        m_btnEstimateNoise = makeSidebarToolButton(u"Estimate Noise\u2026"_s);
        m_btnMomentMap     = makeSidebarToolButton(u"Moment Map\u2026"_s);
        m_btnLineWidth     = makeSidebarToolButton(u"Line-width Map\u2026"_s);
        m_btnBaselineSub   = makeSidebarToolButton(u"Baseline Subtraction\u2026"_s);
        m_btnStackCubes    = makeSidebarToolButton(u"Stack Spectral Cubes\u2026"_s);
        m_btnPvDiagram     = makeSidebarToolButton(u"PV Diagram"_s);
        cl->addWidget(m_btnEstimateNoise);
        cl->addWidget(m_btnMomentMap);
        cl->addWidget(m_btnLineWidth);
        cl->addWidget(m_btnBaselineSub);
        cl->addWidget(m_btnStackCubes);
        cl->addWidget(m_btnPvDiagram);
    }

    // REGIONS group: drawing tools -- pick a shape, compute statistics +
    // spectral profile.
    {
        auto *cl = makeGroup(u"REGIONS"_s);
        m_btnBoxRegion     = makeSidebarToolButton(u"Box Region"_s);
        m_btnCircleRegion  = makeSidebarToolButton(u"Circle Region"_s);
        m_btnPolygonRegion = makeSidebarToolButton(u"Polygon Region"_s);
        m_btnAnnulusRegion = makeSidebarToolButton(u"Annulus Region"_s);
        cl->addWidget(m_btnBoxRegion);
        cl->addWidget(m_btnCircleRegion);
        cl->addWidget(m_btnPolygonRegion);
        cl->addWidget(m_btnAnnulusRegion);
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
        l->setStyleSheet(kInspectorKvValueStyle());
        l->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        l->setWordWrap(false);
        return l;
    };

    // Build one flat section ("State", "Statistics", "WCS") with a 1px
    // bottom hairline divider, matching the Observatory Console design.
    auto makeSection = [](const QString &title, QWidget *parent)
            -> std::pair<QFrame *, QFormLayout *> {
        auto *section = new QFrame(parent);
        section->setObjectName(QStringLiteral("inspectorSection"));
        section->setStyleSheet(kInspectorSectionStyle());

        auto *col = new QVBoxLayout(section);
        col->setContentsMargins(0, 10, 0, 10);
        col->setSpacing(6);

        auto *titleLbl = new QLabel(title, section);
        titleLbl->setStyleSheet(kInspectorTitleStyle());
        col->addWidget(titleLbl);

        auto *fl = new QFormLayout();
        fl->setContentsMargins(0, 0, 0, 0);
        fl->setHorizontalSpacing(12);
        fl->setVerticalSpacing(4);
        fl->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        fl->setFormAlignment(Qt::AlignTop);
        fl->setRowWrapPolicy(QFormLayout::DontWrapRows);
        fl->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
        col->addLayout(fl);

        return { section, fl };
    };

    auto addRow = [](QFormLayout *fl, const QString &k, QWidget *v) {
        auto *lbl = new QLabel(k);
        lbl->setStyleSheet(kInspectorKvLabelStyle());
        fl->addRow(lbl, v);
    };

    auto *page = new QWidget();
    auto *root = new QVBoxLayout(page);
    // Match the symmetric horizontal margins of the other sidebar pages
    // (3D View Settings / 2D View Settings / Tools / Cube Extras all use 8 px)
    // so the rail-to-content gap doesn't visually shift between tabs.
    root->setContentsMargins(8, 0, 8, 0);
    root->setSpacing(0);

    // ── 1. DATASET SUMMARY ────────────────────────────────────────────────────
    // File header: badge + filename + dimensions (no card box).
    {
        auto *header = new QFrame(page);
        header->setObjectName(QStringLiteral("inspectorSection"));
        header->setStyleSheet(kInspectorSectionStyle());

        auto *col = new QVBoxLayout(header);
        col->setContentsMargins(0, 14, 0, 12);
        col->setSpacing(4);

        auto *top = new QHBoxLayout();
        top->setContentsMargins(0, 0, 0, 0);
        top->setSpacing(8);

        auto *badge = new QLabel(u"CUBE"_s, header);
        badge->setStyleSheet(kInspectorBadgeStyle());
        badge->setAlignment(Qt::AlignCenter);
        badge->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
        top->addWidget(badge);

        m_infoDsName = new QLabel(QFileInfo(info.filepath).fileName(), header);
        m_infoDsName->setStyleSheet(QStringLiteral(
                    "QLabel { color: %1; font-size: 13px; font-weight: 700; border: none; }")
                    .arg(QLatin1StringView(VisivoTheme::kOnSurface())));
        m_infoDsName->setWordWrap(true);
        top->addWidget(m_infoDsName, 1);

        col->addLayout(top);

        m_infoDsDimensions = new QLabel(
                u"%1 \u00d7 %2 \u00d7 %3"_s
                        .arg(info.datasetWidth)
                        .arg(info.datasetHeight)
                        .arg(info.datasetDepth),
                header);
        m_infoDsDimensions->setStyleSheet(QStringLiteral(
                    "QLabel { color: %1; font-family: Menlo, Consolas, monospace;"
                    "  font-size: 10px; border: none; }")
                    .arg(QLatin1StringView(VisivoTheme::kSecondary())));
        col->addWidget(m_infoDsDimensions);

        root->addWidget(header);
    }

    // ── 2. SYSTEM / STATUS ────────────────────────────────────────────────────
    // State section: load progress + render mode + 2D mode + moment status.
    {
        auto [section, fl] = makeSection(u"State"_s, page);

        m_infoDsLoadState = makeVal(u"Preview"_s);
        m_infoRenderMode  = makeVal(u"Isosurface"_s);
        m_info2dMode      = makeVal(u"Slice"_s);
        m_infoMomentState = makeVal(u"\u2014"_s);

        m_infoDsLoadState->setToolTip(u"Preview, loading full resolution, or full resolution."_s);
        m_infoRenderMode->setToolTip(u"Current 3D rendering mode (Isosurface or Volume Rendering)."_s);
        m_info2dMode->setToolTip(u"Current 2D view mode (Slice or Moment Map)."_s);
        m_infoMomentState->setToolTip(u"State of the last moment map computation."_s);

        addRow(fl, u"Load state"_s, m_infoDsLoadState);
        addRow(fl, u"3D mode"_s,    m_infoRenderMode);
        addRow(fl, u"2D mode"_s,    m_info2dMode);
        addRow(fl, u"Moment map"_s, m_infoMomentState);

        root->addWidget(section);
    }

    // ── 3. DATA STATISTICS ────────────────────────────────────────────────────
    // The QLineEdit widgets (lineImgMin, lineImgMax, etc.) are defined in the UI
    // file and updated via ui-> calls in vtkWindowCube.cpp.  We reparent them
    // into new key/value rows so values are never truncated in a half-width cell.
    {
        // Helper: detach all layout items from a frame's existing layout (the one
        // created by setupUi) and return the cleared frame so we can install a
        // fresh QFormLayout on it.
        auto clearFrameLayout = [](QFrame *frame) {
            if (auto *old = frame->layout()) {
                while (old->count())
                    delete old->takeAt(0); // deletes QLayoutItem, not the widget
                delete old;
            }
        };

        static const QString kRoLineEdit =
                u"QLineEdit { background: transparent; border: none;"
                u"  color: %1; font-family: Menlo, Consolas, monospace;"
                u"  font-size: 12px; padding: 0; }"_s;
        const QString roCss = QString(kRoLineEdit)
                .arg(QLatin1StringView(VisivoTheme::kOnSurface()));

        auto [section, fl] = makeSection(u"Statistics"_s, page);

        // ── 2D image sub-section ─────────────────────────────────────────────
        auto *imgSub = new QLabel(u"2D image"_s);
        imgSub->setStyleSheet(kInspectorSubLabelStyle());
        // Insert sub-header above the form layout so the section title +
        // sub-header + KV rows read top-to-bottom.
        fl->addRow(imgSub);
        m_ui->labelImg->hide();   // legacy header, replaced by `imgSub`
        m_ui->frameImgStats->hide();  // legacy frame; we reuse only its line-edits

        clearFrameLayout(m_ui->frameImgStats);
        for (auto *lbl : { m_ui->labelImgMin, m_ui->labelImgMax,
                           m_ui->labelImgMean, m_ui->labelImgRms })
            lbl->hide();
        for (auto *le : { m_ui->lineImgMin, m_ui->lineImgMax,
                          m_ui->lineImgMean, m_ui->lineImgRms }) {
            le->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            le->setStyleSheet(roCss);
            le->setFrame(false);
            le->setParent(section);
        }
        addRow(fl, u"Min"_s,  m_ui->lineImgMin);
        addRow(fl, u"Max"_s,  m_ui->lineImgMax);
        addRow(fl, u"Mean"_s, m_ui->lineImgMean);
        addRow(fl, u"RMS"_s,  m_ui->lineImgRms);

        // ── 3D cube sub-section ──────────────────────────────────────────────
        auto *cubeSub = new QLabel(u"3D cube"_s);
        cubeSub->setStyleSheet(kInspectorSubLabelStyle());
        fl->addRow(cubeSub);
        m_ui->frameCubeStats->hide();   // legacy frame; we keep only its line-edits

        clearFrameLayout(m_ui->frameCubeStats);
        for (auto *lbl : { m_ui->labelCubeMin, m_ui->labelCubeMax,
                           m_ui->labelCubeMean, m_ui->labelCubeRms })
            lbl->hide();
        for (auto *le : { m_ui->lineCubeMin, m_ui->lineCubeMax,
                          m_ui->lineCubeMean, m_ui->lineCubeRms }) {
            le->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            le->setStyleSheet(roCss);
            le->setFrame(false);
            le->setParent(section);
        }
        addRow(fl, u"Min"_s,  m_ui->lineCubeMin);
        addRow(fl, u"Max"_s,  m_ui->lineCubeMax);
        addRow(fl, u"Mean"_s, m_ui->lineCubeMean);
        addRow(fl, u"RMS"_s,  m_ui->lineCubeRms);

        root->addWidget(section);
    }

    // WCS section: status badge + reference frame + spectral axis.
    {
        auto [section, fl] = makeSection(u"WCS"_s, page);

        m_infoWcsBadge = makeVal();
        m_infoWcsFrame = makeVal(info.wcsFrameLabel);
        m_infoSpectral = makeVal(info.spectralAxisTitle);

        m_infoWcsBadge->setToolTip(u"World Coordinate System status for this dataset."_s);
        m_infoWcsFrame->setToolTip(u"The celestial reference frame currently in use."_s);
        m_infoSpectral->setToolTip(u"The spectral axis type and units for the third dimension."_s);

        // Populate WCS badge with initial status. Colours: green = ok,
        // amber = degraded, ochre = repaired (theme-invariant accents).
        if (info.wcsStatus.compare(u"ok"_s, Qt::CaseInsensitive) == 0) {
            m_infoWcsBadge->setText(u"OK"_s);
            m_infoWcsBadge->setStyleSheet(u"QLabel { font-size: 12px; color: #5DA87A;"
                                          u" font-family: Menlo, Consolas, monospace; }"_s);
        } else if (info.wcsStatus.compare(u"degraded"_s, Qt::CaseInsensitive) == 0) {
            m_infoWcsBadge->setText(u"Degraded"_s);
            m_infoWcsBadge->setStyleSheet(u"QLabel { font-size: 12px; color: #E8A840;"
                                          u" font-family: Menlo, Consolas, monospace; }"_s);
        } else {
            m_infoWcsBadge->setText(u"Repaired"_s);
            m_infoWcsBadge->setStyleSheet(u"QLabel { font-size: 12px; color: #B77610;"
                                          u" font-family: Menlo, Consolas, monospace; }"_s);
        }
        if (!info.wcsWarningMessage.isEmpty())
            m_infoWcsBadge->setToolTip(info.wcsWarningMessage);

        addRow(fl, u"Status"_s,        m_infoWcsBadge);
        addRow(fl, u"Frame"_s,         m_infoWcsFrame);
        addRow(fl, u"Spectral axis"_s, m_infoSpectral);

        root->addWidget(section);
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
                                      QAction *cameraRoi,
                                      QAction *momentMap,
                                      QAction *linewidthMap,
                                      QAction *baselineSub,
                                      QAction *stackCubes,
                                      QAction *openInVR,
                                      QAction *wcsAxes,
                                      QAction *boxRegion,
                                      QAction *circleRegion,
                                      QAction *polygonRegion,
                                      QAction *annulusRegion)
{
    // setDefaultAction() links button text, tooltip, enabled state, and
    // checkability to the action — keeps the sidebar and menu in sync.

    // ── EXPLORATION buttons ──────────────────────────────────────────────────
    if (m_btnDataProbe && extractSpectrum) {
        m_btnDataProbe->setDefaultAction(extractSpectrum);
        m_btnDataProbe->setToolTip(u"Probe data values at a point in the 2D view."_s);
    }
    if (m_btnOpenInVR && openInVR)
        m_btnOpenInVR->setDefaultAction(openInVR);

    // ── ANALYSIS buttons ─────────────────────────────────────────────────────
    if (m_btnEstimateNoise && estimateNoise)
        m_btnEstimateNoise->setDefaultAction(estimateNoise);
    if (m_btnMomentMap && momentMap)
        m_btnMomentMap->setDefaultAction(momentMap);
    if (m_btnLineWidth && linewidthMap)
        m_btnLineWidth->setDefaultAction(linewidthMap);
    if (m_btnBaselineSub && baselineSub)
        m_btnBaselineSub->setDefaultAction(baselineSub);
    if (m_btnStackCubes && stackCubes)
        m_btnStackCubes->setDefaultAction(stackCubes);
    if (m_btnPvDiagram && extractPvDiagram) {
        m_btnPvDiagram->setDefaultAction(extractPvDiagram);
        m_btnPvDiagram->setToolTip(
                u"Draw a path in the 2D view to extract a position-velocity diagram."_s);
    }

    // ── REGIONS buttons ──────────────────────────────────────────────────────
    if (m_btnBoxRegion && boxRegion) {
        m_btnBoxRegion->setDefaultAction(boxRegion);
        m_btnBoxRegion->setToolTip(u"Draw a box region and compute statistics + spectral profile."_s);
    }
    if (m_btnCircleRegion && circleRegion) {
        m_btnCircleRegion->setDefaultAction(circleRegion);
        m_btnCircleRegion->setToolTip(u"Draw a circle region and compute statistics + spectral profile."_s);
    }
    if (m_btnPolygonRegion && polygonRegion) {
        m_btnPolygonRegion->setDefaultAction(polygonRegion);
        m_btnPolygonRegion->setToolTip(u"Draw a polygon region and compute statistics + spectral profile."_s);
    }
    if (m_btnAnnulusRegion && annulusRegion) {
        m_btnAnnulusRegion->setDefaultAction(annulusRegion);
        m_btnAnnulusRegion->setToolTip(u"Draw an annulus region and compute statistics + spectral profile."_s);
    }

    // ── 3D View Settings page buttons ────────────────────────────────────────
    if (m_btnCameraRoi && cameraRoi)
        m_btnCameraRoi->setDefaultAction(cameraRoi);

    // ── 2D View Settings page buttons ────────────────────────────────────────
    if (m_btnWcsAxes && wcsAxes)
        m_btnWcsAxes->setDefaultAction(wcsAxes);
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
    auto *lbl = m_momentInfoLabel;
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
    if (auto *l = m_infoDsLoadState) {
        l->setText(data.loadStateText);
        l->setStyleSheet(data.loadStateStyleSheet);
    }
    if (auto *l = m_infoWcsFrame)
        l->setText(data.wcsFrame);
    if (auto *l = m_infoSpectral)
        l->setText(data.spectralAxis);
    if (auto *badge = m_infoWcsBadge) {
        badge->setText(data.wcsBadgeText);
        badge->setStyleSheet(data.wcsBadgeStyleSheet);
        if (!data.wcsBadgeTooltip.isEmpty())
            badge->setToolTip(data.wcsBadgeTooltip);
    }
    if (auto *l = m_infoRenderMode)
        l->setText(data.renderModeText);
    if (auto *l = m_info2dMode)
        l->setText(data.imageModeText);
    if (auto *l = m_infoMomentState) {
        l->setText(data.momentStateText);
        l->setStyleSheet(data.momentStateStyleSheet);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// initStatusBarLabels  — creates the three status-bar labels and registers them
// ─────────────────────────────────────────────────────────────────────────────

void CubeUiAssembler::initStatusBarLabels(QStatusBar *bar)
{
    m_dataStateLabel = new QLabel();
    m_dataStateLabel->setStyleSheet(u"QLabel { padding-left: 8px; color: palette(window-text); }"_s);
    m_dataStateLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    bar->addPermanentWidget(m_dataStateLabel);

    m_sanityLabel = new QLabel();
    m_sanityLabel->setStyleSheet(u"QLabel { padding-left: 8px; }"_s);
    m_sanityLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    bar->addPermanentWidget(m_sanityLabel);

    m_wcsStatusLabel = new QLabel();
    m_wcsStatusLabel->setStyleSheet(u"QLabel { padding-left: 8px; font-weight: 600; }"_s);
    m_wcsStatusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    bar->addPermanentWidget(m_wcsStatusLabel);
}

// ─────────────────────────────────────────────────────────────────────────────
// updateWcsStatus  — pure label rendering from pre-built WcsStatusData
// ─────────────────────────────────────────────────────────────────────────────

void CubeUiAssembler::updateWcsStatus(const WcsStatusData &data)
{
    auto *lbl = m_wcsStatusLabel;
    if (!lbl)
        return;
    if (!data.visible) {
        lbl->clear();
        lbl->setToolTip({});
        lbl->hide();
        return;
    }
    lbl->setText(data.text);
    lbl->setStyleSheet(data.styleSheet);
    lbl->setToolTip(data.tooltip);
    lbl->show();
}

// ─────────────────────────────────────────────────────────────────────────────
// updateSanityStatus  — pure label rendering from pre-built SanityStatusData
// ─────────────────────────────────────────────────────────────────────────────

void CubeUiAssembler::updateSanityStatus(const SanityStatusData &data)
{
    auto *lbl = m_sanityLabel;
    if (!lbl)
        return;
    lbl->setText(data.text);
    lbl->setToolTip(data.tooltip);
}

// ─────────────────────────────────────────────────────────────────────────────
// updateDataState  — pure label rendering from pre-built DataStateUiData
// ─────────────────────────────────────────────────────────────────────────────

void CubeUiAssembler::updateDataState(const DataStateUiData &data)
{
    auto *lbl = m_dataStateLabel;
    if (!lbl)
        return;
    lbl->setText(data.text);
    lbl->setToolTip(data.tooltip);
}
