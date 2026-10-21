#include "ChannelOverlay.h"
#include "theme/VisivoTheme.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QResizeEvent>
#include <QShowEvent>
#include <QSlider>
#include <algorithm>
#include <cmath>
#include <QFontMetrics>
#include <QRegularExpression>

using namespace Qt::StringLiterals;

namespace {
// Margin between the overlay and the cube widget edges (matches the
// `bottom: 10, left: 10, right: 10` values in cubeviewer.jsx).
constexpr int kOverlayMargin = 10;
constexpr int kOverlayHeight = 28;
}

ChannelOverlay::ChannelOverlay(QWidget *parent)
    : QFrame(parent)
{
    setObjectName(QStringLiteral("channelOverlay"));
    setAttribute(Qt::WA_StyledBackground, true);
    setFocusPolicy(Qt::ClickFocus);
    setMinimumHeight(kOverlayHeight);

    auto *row = new QHBoxLayout(this);
    row->setContentsMargins(10, 0, 10, 0);
    row->setSpacing(10);

    m_chLabel = new QLabel(u"CH"_s, this);
    m_chValue = new QLabel(u"1"_s, this);
    m_slider  = new QSlider(Qt::Horizontal, this);
    m_vLabel  = new QLabel(u"v"_s, this);
    m_vValue  = new QLabel(QString(), this);

    m_chValue->setMinimumWidth(38);
    m_chValue->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    m_slider->setRange(1, 1);
    m_slider->setSingleStep(1);
    m_slider->setPageStep(10);
    m_slider->setTracking(true);
    m_slider->setMinimumHeight(20);
    // The slider is the control; it keeps room to be dragged in a narrow pane (a
    // 4-pane layout), and the spectral value yields instead — a long frequency
    // ("1414870370.37 Hz") had squeezed the slider down to its handle.
    m_slider->setMinimumWidth(70);

    m_vValue->setMinimumWidth(40);
    m_vValue->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_vValue->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    row->addWidget(m_chLabel);
    row->addWidget(m_chValue);
    row->addWidget(m_slider, /*stretch=*/1);
    row->addWidget(m_vLabel);
    row->addWidget(m_vValue, /*stretch=*/1);

    applyStyle();

    QObject::connect(m_slider, &QSlider::valueChanged, this, [this](int v) {
        m_chValue->setText(QString::number(v));
        emit channelChanged(v);
    });
}

void ChannelOverlay::setRange(int minChannel, int maxChannel)
{
    if (maxChannel < minChannel) {
        maxChannel = minChannel;
    }
    const QSignalBlocker block(m_slider);
    m_slider->setMinimum(minChannel);
    m_slider->setMaximum(maxChannel);
}

void ChannelOverlay::setChannel(int channel)
{
    channel = std::clamp(channel, m_slider->minimum(), m_slider->maximum());
    if (m_slider->value() != channel) {
        const QSignalBlocker block(m_slider);
        m_slider->setValue(channel);
    }
    m_chValue->setText(QString::number(channel));
}

void ChannelOverlay::setSpectralLabel(const QString &textWithUnit)
{
    m_spectralFull = textWithUnit;
    m_vValue->setToolTip(textWithUnit);
    refreshSpectralLabel();
}

void ChannelOverlay::refreshSpectralLabel()
{
    m_vValue->setText(fitSpectralLabel(m_spectralFull, m_vValue->fontMetrics(),
                                       std::max(0, m_vValue->width())));
}

void ChannelOverlay::resizeEvent(QResizeEvent *event)
{
    QFrame::resizeEvent(event);
    refreshSpectralLabel();
}

QString ChannelOverlay::fitSpectralLabel(const QString &full, const QFontMetrics &fm, int available)
{
    if (available <= 0 || fm.horizontalAdvance(full) <= available)
        return full;
    // "<number> Hz" → GHz / MHz with the significant digits a channel needs.
    static const QRegularExpression hz(QStringLiteral("^\\s*([-+]?[0-9.]+(?:[eE][-+]?[0-9]+)?)\\s*Hz\\s*$"));
    const auto m = hz.match(full);
    if (m.hasMatch()) {
        bool ok = false;
        const double v = m.captured(1).toDouble(&ok);
        if (ok) {
            const QString compact = std::abs(v) >= 1e9
                    ? QStringLiteral("%1 GHz").arg(v / 1e9, 0, 'f', 6)
                    : std::abs(v) >= 1e6 ? QStringLiteral("%1 MHz").arg(v / 1e6, 0, 'f', 4)
                                         : full;
            if (fm.horizontalAdvance(compact) <= available)
                return compact;
            return fm.elidedText(compact, Qt::ElideRight, available);
        }
    }
    return fm.elidedText(full, Qt::ElideRight, available);
}

int ChannelOverlay::channel() const
{
    return m_slider->value();
}

bool ChannelOverlay::eventFilter(QObject *obj, QEvent *event)
{
    if (obj == parent()
        && (event->type() == QEvent::Resize || event->type() == QEvent::Show)) {
        repositionOverParent();
    }
    return QFrame::eventFilter(obj, event);
}

int ChannelOverlay::reservedHeight()
{
    return kOverlayHeight + 2 * kOverlayMargin;
}

void ChannelOverlay::repositionOverParent()
{
    auto *p = parentWidget();
    if (!p) return;
    const int pw = p->width();
    const int ph = p->height();
    const int x  = kOverlayMargin;
    const int y  = ph - kOverlayHeight - kOverlayMargin;
    const int w  = std::max(80, pw - 2 * kOverlayMargin);
    setGeometry(x, y, w, kOverlayHeight);
    raise();   // keep above the VTK render surface
}

void ChannelOverlay::applyStyle()
{
    // Translucent backdrop with hairline border + rounded corners. Brand-blue
    // primary on the slider track + handle. Mono fonts on the readouts so the
    // numbers stay aligned at all widths.
    const QString isDarkBg = VisivoTheme::isDark()
            ? QStringLiteral("rgba(0, 0, 0, 0.55)")
            : QStringLiteral("rgba(255, 255, 255, 0.78)");
    const QString muted = QString::fromLatin1(VisivoTheme::kOnSurfaceVariant());
    const QString fg    = QString::fromLatin1(VisivoTheme::kOnSurface());
    const QString line  = QString::fromLatin1(VisivoTheme::kOutline());
    const QString surf1 = QString::fromLatin1(VisivoTheme::kSurfaceContainer());
    const QString surf3 = QString::fromLatin1(VisivoTheme::kSurfaceContainerHighest());

    setStyleSheet(QStringLiteral(
                          "QFrame#channelOverlay {"
                          "  background: %1;"
                          "  border: 1px solid %2;"
                          "  border-radius: 6px;"
                          "}")
                          .arg(isDarkBg, line));

    const QString monoSm = QStringLiteral(
            "QLabel { color: %1; font-family: Menlo, Consolas, monospace;"
            "  font-size: 10px; border: none; background: transparent; }");
    const QString monoMd = QStringLiteral(
            "QLabel { color: %1; font-family: Menlo, Consolas, monospace;"
            "  font-size: 11px; border: none; background: transparent; }");

    m_chLabel->setStyleSheet(monoSm.arg(muted));
    m_chValue->setStyleSheet(monoMd.arg(fg));
    m_vLabel ->setStyleSheet(monoSm.arg(muted));
    m_vValue ->setStyleSheet(monoMd.arg(fg));

    m_slider->setStyleSheet(QStringLiteral(
            "QSlider::groove:horizontal {"
            "  height: 4px; background: %1; border-radius: 2px; border: none;"
            "}"
            "QSlider::sub-page:horizontal {"
            "  background: #048ABF; border-radius: 2px;"
            "}"
            "QSlider::add-page:horizontal {"
            "  background: %1; border-radius: 2px;"
            "}"
            "QSlider::handle:horizontal {"
            "  background: #048ABF; width: 12px; height: 12px;"
            "  border: 2px solid %2; border-radius: 6px;"
            "  margin: -5px 0;"
            "}"
            "QSlider::handle:horizontal:hover {"
            "  background: #5BB8D8;"
            "}").arg(surf3, surf1));
}
