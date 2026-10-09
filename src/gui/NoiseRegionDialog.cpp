#include "NoiseRegionDialog.h"

#include "theme/ToolDialogStyle.h"
#include "theme/VisivoTheme.h"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

using namespace Qt::StringLiterals;

NoiseRegionDialog::NoiseRegionDialog(int width, int height, int depth, QWidget *parent)
    : QDialog(parent)
    , m_width(width)
    , m_height(height)
    , m_depth(depth)
{
    setWindowTitle(u"Noise Estimation, Select Region"_s);
    // NOT modal. This window now stays open across the estimate and past it, and
    // an application-modal settings window blocks the very result window it
    // produces: ToolResultDialog is a non-modal sibling parented to the cube, so
    // the numbers appeared behind a window the user could not dismiss without
    // losing them (reproduced in a Qt harness). The progress sheet
    // supplies the blocking that the run itself needs, and only while it runs.
    setModal(false);
    // Shared input theme — see ToolDialogStyle.
    setStyleSheet(ToolDialogStyle::inputCss());

    const int xMax = std::max(0, width  - 1);
    const int yMax = std::max(0, height - 1);
    const int zMax = std::max(0, depth  - 1);
    const QString tMuted  = QString::fromLatin1(VisivoTheme::kOnSurfaceVariant());
    const QString tBorder = QString::fromLatin1(VisivoTheme::kOutline());

    auto makeSpinBox = [&](int lo, int hi, int val) -> QSpinBox * {
        auto *sb = new QSpinBox(this);
        sb->setRange(lo, hi);
        sb->setValue(val);
        sb->setMinimumWidth(80);
        sb->setAlignment(Qt::AlignRight);
        return sb;
    };
    // Compact start→end row: two spinboxes separated by a muted arrow, so
    // the form takes one line per axis instead of two (x min + x max).
    auto makeRangeRow = [&](QSpinBox *lo, QSpinBox *hi) -> QWidget * {
        auto *w = new QWidget(this);
        auto *h = new QHBoxLayout(w);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(8);
        h->addWidget(lo, 1);
        auto *arrow = new QLabel(u"→"_s, w);
        arrow->setStyleSheet(QStringLiteral("color: %1; font-weight: 600;").arg(tMuted));
        h->addWidget(arrow);
        h->addWidget(hi, 1);
        return w;
    };

    // ── Root layout + breadcrumb + intro ─────────────────────────────────
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(16, 14, 16, 14);
    root->setSpacing(10);

    root->addWidget(ToolDialogStyle::makeBreadcrumb(this, u"Estimate Noise"_s));

    auto *intro = new QLabel(
            u"Pick a region of the cube where the signal is line-free - the "
            u"backend computes a robust noise estimate over it. Defaults to "
            u"the full cube extent."_s, this);
    intro->setStyleSheet(ToolDialogStyle::helperCss());
    intro->setWordWrap(true);
    root->addWidget(intro);

    // Hidden until setRegion() restores a previously-saved region for this cube.
    m_restoredHint = new QLabel(u"↺ Restored from your last session for this cube."_s, this);
    m_restoredHint->setStyleSheet(QStringLiteral("color: %1; font-style: italic;").arg(tMuted));
    m_restoredHint->setWordWrap(true);
    m_restoredHint->hide();
    root->addWidget(m_restoredHint);

    // ── SPATIAL REGION ───────────────────────────────────────────────────
    auto *spatialForm = ToolDialogStyle::addSection(root, u"Spatial region (pixels)"_s, this);

    m_x0Spin = makeSpinBox(0, xMax, 0);
    m_x1Spin = makeSpinBox(0, xMax, xMax);
    m_y0Spin = makeSpinBox(0, yMax, 0);
    m_y1Spin = makeSpinBox(0, yMax, yMax);

    spatialForm->addRow(u"X (columns)"_s, makeRangeRow(m_x0Spin, m_x1Spin));
    spatialForm->addRow(u"Y (rows)"_s,    makeRangeRow(m_y0Spin, m_y1Spin));

    m_spatialHint = new QLabel(this);
    m_spatialHint->setStyleSheet(ToolDialogStyle::helperCss());
    m_spatialHint->setWordWrap(true);
    spatialForm->addRow(QString(), m_spatialHint);

    // ── SPECTRAL RANGE ───────────────────────────────────────────────────
    auto *spectralForm = ToolDialogStyle::addSection(root, u"Spectral range (channels)"_s, this);

    m_chStartSpin = makeSpinBox(0, zMax, 0);
    m_chEndSpin   = makeSpinBox(0, zMax, zMax);

    spectralForm->addRow(u"Start → End"_s, makeRangeRow(m_chStartSpin, m_chEndSpin));

    m_spectralHint = new QLabel(this);
    m_spectralHint->setStyleSheet(ToolDialogStyle::helperCss());
    m_spectralHint->setWordWrap(true);
    spectralForm->addRow(QString(), m_spectralHint);

    // ── METHODOLOGY footer ───────────────────────────────────────────────
    // Combines two pieces of static methodology context into a single muted
    // line so users know how the noise is computed (MAD, blanks excluded)
    // without having to open the docs.
    auto *divider = new QFrame(this);
    divider->setFrameShape(QFrame::HLine);
    divider->setStyleSheet(QStringLiteral("color: %1;").arg(tBorder));
    root->addWidget(divider);

    auto *methodLabel = new QLabel(
            u"σ = 1.4826 · MAD per channel (robust 1σ for Gaussian noise) · "
            u"zero / NaN-blanked pixels excluded automatically when they "
            u"dominate the selection."_s, this);
    methodLabel->setStyleSheet(ToolDialogStyle::helperCss());
    methodLabel->setWordWrap(true);
    root->addWidget(methodLabel);

    // ── BUTTONS: Reset (left) + Close (secondary) + Estimate (primary
    // brand-blue). VisivoTheme buttons match every other tool dialog. ────
    auto *btnRow = new QHBoxLayout();
    auto *resetBtn = VisivoTheme::makeSecondaryButton(u"Reset to Full Extent"_s, this);
    btnRow->addWidget(resetBtn);
    btnRow->addStretch();
    auto *closeBtn = VisivoTheme::makeSecondaryButton(u"Close"_s, this);
    m_okBtn        = VisivoTheme::makePrimaryButton(u"Estimate"_s, this);
    m_okBtn->setDefault(true);
    btnRow->addWidget(closeBtn);
    btnRow->addWidget(m_okBtn);
    root->addLayout(btnRow);

    // Estimate does NOT accept: the window stays open across the compute, carries
    // the progress sheet and is ready for the next region, like every other tool
    // dialog. The controller listens for this.
    QObject::connect(m_okBtn,   &QPushButton::clicked, this, &NoiseRegionDialog::estimateRequested);
    QObject::connect(closeBtn,  &QPushButton::clicked, this, &QDialog::reject);
    QObject::connect(resetBtn,  &QPushButton::clicked, this, &NoiseRegionDialog::resetToFullExtent);

    // Wire live validation + live preview emission. All six spinboxes (4
    // spatial + 2 spectral) notify the cube viewer via regionPreviewChanged
    // so it can keep the 2D slice rectangle AND the 3D volume box in sync
    // with the user's selection while they tune the bounds.
    const auto onAny = [this]() {
        this->validate();
        this->emitRegion();
    };
    QObject::connect(m_x0Spin,      &QSpinBox::valueChanged, this, onAny);
    QObject::connect(m_x1Spin,      &QSpinBox::valueChanged, this, onAny);
    QObject::connect(m_y0Spin,      &QSpinBox::valueChanged, this, onAny);
    QObject::connect(m_y1Spin,      &QSpinBox::valueChanged, this, onAny);
    QObject::connect(m_chStartSpin, &QSpinBox::valueChanged, this, onAny);
    QObject::connect(m_chEndSpin,   &QSpinBox::valueChanged, this, onAny);

    validate();
    // Emit once at construction so the cube viewer paints the initial
    // (full-extent) overlays right away when the dialog opens.
    emitRegion();
}

void NoiseRegionDialog::emitRegion()
{
    emit regionPreviewChanged(m_x0Spin->value(), m_x1Spin->value(),
                              m_y0Spin->value(), m_y1Spin->value(),
                              m_chStartSpin->value(), m_chEndSpin->value());
}

void NoiseRegionDialog::validate()
{
    const int x0 = m_x0Spin->value(), x1 = m_x1Spin->value();
    const int y0 = m_y0Spin->value(), y1 = m_y1Spin->value();
    const int c0 = m_chStartSpin->value(), c1 = m_chEndSpin->value();
    const bool xOk = (x0 <= x1), yOk = (y0 <= y1), cOk = (c0 <= c1);
    // Never available while an estimate is in flight, whatever the bounds say.
    m_okBtn->setEnabled(xOk && yOk && cOk && !m_busy);

    // Spatial hint: pixel count + "ratio of footprint" + range warning.
    if (m_spatialHint) {
        if (!xOk || !yOk) {
            m_spatialHint->setText(u"⚠ start must be ≤ end on both axes."_s);
        } else {
            const qint64 wPx = qint64(x1) - x0 + 1;
            const qint64 hPx = qint64(y1) - y0 + 1;
            const qint64 nPx = wPx * hPx;
            const qint64 totalPx = qint64(std::max(1, m_width))
                                  * qint64(std::max(1, m_height));
            const int pct = totalPx > 0
                            ? static_cast<int>((100 * nPx) / totalPx) : 0;
            m_spatialHint->setText(
                    u"%1 × %2 pixels (%3 total, %4% of the cube footprint). "
                    u"Pick an off-source area inside the data footprint - "
                    u"avoid blanked borders or the result is mostly zero."_s
                            .arg(wPx).arg(hPx)
                            .arg(QLocale::system().toString(nPx)).arg(pct));
        }
    }

    // Spectral hint: channel count + line-free vs full-range guidance.
    if (m_spectralHint) {
        if (!cOk) {
            m_spectralHint->setText(u"⚠ start must be ≤ end."_s);
        } else {
            const int n = c1 - c0 + 1;
            const int total = std::max(1, m_depth);
            m_spectralHint->setText(
                    u"%1 of %2 channels selected. Restrict to line-free "
                    u"channels for a clean σ; leave full range for a per-"
                    u"channel σ across the whole bandpass."_s
                            .arg(n).arg(total));
        }
    }
}

void NoiseRegionDialog::resetToFullExtent()
{
    m_x0Spin->setValue(0);
    m_x1Spin->setValue(std::max(0, m_width  - 1));
    m_y0Spin->setValue(0);
    m_y1Spin->setValue(std::max(0, m_height - 1));
    m_chStartSpin->setValue(0);
    m_chEndSpin->setValue(std::max(0, m_depth - 1));
}

int NoiseRegionDialog::x0()          const { return m_x0Spin->value(); }
int NoiseRegionDialog::x1()          const { return m_x1Spin->value(); }
int NoiseRegionDialog::y0()          const { return m_y0Spin->value(); }
int NoiseRegionDialog::y1()          const { return m_y1Spin->value(); }
int NoiseRegionDialog::channelStart() const { return m_chStartSpin->value(); }
int NoiseRegionDialog::channelEnd()   const { return m_chEndSpin->value(); }

void NoiseRegionDialog::setRegion(int x0, int x1, int y0, int y1, int chStart, int chEnd)
{
    // The spinboxes clamp out-of-range values to the cube extent. Setting them
    // fires the valueChanged connections → validate() + emitRegion() run, so the
    // OK-enabled state and the live preview reflect the restored region.
    m_x0Spin->setValue(x0);
    m_x1Spin->setValue(x1);
    m_y0Spin->setValue(y0);
    m_y1Spin->setValue(y1);
    m_chStartSpin->setValue(chStart);
    m_chEndSpin->setValue(chEnd);
    if (m_restoredHint) {
        m_restoredHint->show();   // called only when restoring a saved region
    }
}

void NoiseRegionDialog::setBusy(bool busy)
{
    // Estimate is available only when NOT busy AND the region is valid. Setting
    // it straight from `busy` made completion re-enable the button over invalid
    // bounds, and validate() firing during a run re-enabled it mid-compute
    //. The two conditions live in one place: validate().
    m_busy = busy;
    validate();
}
