#include "ToolResultDialog.h"
#include "theme/VisivoTheme.h"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QScreen>
#include <QScrollArea>
#include <QShowEvent>
#include <QVBoxLayout>
#include <QWindow>

using namespace Qt::StringLiterals;

namespace {
// Helpers that return Layout B-tinted theme colors as QStrings, so the QSS
// templates below stay readable.
QString tBg()         { return QString::fromLatin1(VisivoTheme::kSurface()); }
QString tHeaderBg()   { return QString::fromLatin1(VisivoTheme::kSurfaceContainerLow()); }
QString tBorder()     { return QString::fromLatin1(VisivoTheme::kOutline()); }
QString tTitle()      { return QString::fromLatin1(VisivoTheme::kOnSurface()); }
QString tSubtitle()   { return QString::fromLatin1(VisivoTheme::kOnSurfaceVariant()); }
QString tKey()        { return QString::fromLatin1(VisivoTheme::kOnSurfaceVariant()); }
QString tValue()      { return QString::fromLatin1(VisivoTheme::kOnSurface()); }
QString tGroupTitle() { return QString::fromLatin1(VisivoTheme::kPrimary()); }
}

ToolResultDialog::ToolResultDialog(QWidget *parent)
    : QDialog(parent)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowModality(Qt::NonModal);
    setWindowTitle(u"Analysis Result"_s);
    setMinimumWidth(360);
    setSizeGripEnabled(true);
    // A frameless tool window reads as a "side-sheet" rather than a regular
    // OS dialog — closer to the Layout B intent of attaching analysis output
    // to the viewer rather than spawning loose floating windows.
    setWindowFlag(Qt::Tool, true);

    auto *outerLayout = new QVBoxLayout(this);
    outerLayout->setSpacing(0);
    outerLayout->setContentsMargins(0, 0, 0, 0);

    // Outer container styling (matches Layout B's surface ramp).
    setStyleSheet(
            QStringLiteral("ToolResultDialog { background: %1; }").arg(tBg()));

    // ── Header ───────────────────────────────────────────────────────────────
    auto *headerWidget = new QWidget();
    headerWidget->setObjectName(u"toolResultHeader"_s);
    headerWidget->setStyleSheet(
            u"QWidget#toolResultHeader {"
            u"  background: %1;"
            u"  border-bottom: 1px solid %2;"
            u"}"_s.arg(tHeaderBg(), tBorder()));

    auto *headerLayout = new QVBoxLayout(headerWidget);
    headerLayout->setContentsMargins(16, 12, 16, 12);
    headerLayout->setSpacing(3);

    m_titleLabel = new QLabel();
    m_titleLabel->setStyleSheet(
            u"font-size: 14px; font-weight: bold; color: %1; background: transparent;"_s
                    .arg(tTitle()));
    headerLayout->addWidget(m_titleLabel);

    m_subtitleLabel = new QLabel();
    m_subtitleLabel->setStyleSheet(
            u"font-size: 11px; color: %1; background: transparent;"_s.arg(tSubtitle()));
    m_subtitleLabel->setWordWrap(true);
    m_subtitleLabel->hide();
    headerLayout->addWidget(m_subtitleLabel);

    outerLayout->addWidget(headerWidget);

    // ── Scrollable body ───────────────────────────────────────────────────────
    auto *scrollArea = new QScrollArea();
    scrollArea->setFrameShape(QFrame::NoFrame);
    scrollArea->setWidgetResizable(true);
    scrollArea->setStyleSheet(u"QScrollArea { background: %1; border: none; }"_s.arg(tBg()));

    auto *bodyWidget = new QWidget();
    bodyWidget->setStyleSheet(u"background: %1;"_s.arg(tBg()));
    m_bodyLayout = new QVBoxLayout(bodyWidget);
    m_bodyLayout->setContentsMargins(14, 12, 14, 12);
    m_bodyLayout->setSpacing(10);
    m_bodyLayout->addStretch();   // kept last; addSection inserts before it

    scrollArea->setWidget(bodyWidget);
    outerLayout->addWidget(scrollArea, 1);

    // ── Footer ────────────────────────────────────────────────────────────────
    auto *footer = new QWidget();
    footer->setStyleSheet(
            u"QWidget { background: %1; border-top: 1px solid %2; }"_s
                    .arg(tHeaderBg(), tBorder()));
    auto *footerLayout = new QHBoxLayout(footer);
    footerLayout->setContentsMargins(12, 8, 12, 8);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    QObject::connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::accept);
    footerLayout->addWidget(buttons);

    outerLayout->addWidget(footer);
}

void ToolResultDialog::showEvent(QShowEvent *event)
{
    QDialog::showEvent(event);
    if (m_anchored) return;            // only on first show
    if (!parentWidget()) return;       // free-floating fallback if parentless

    // Anchor as a side-sheet on the right edge of the top-level parent. Both
    // WIDTH and HEIGHT are driven by the content — narrow tools (PV info)
    // stay compact, dense ones (Region Analysis: 8 stat rows + a note)
    // get the room they need without forcing the user to scroll inside the
    // sheet for what is fundamentally a short fixed-size report.
    QWidget *root = parentWidget()->window();
    if (!root) return;
    const QRect parentGeo = root->geometry();

    // Force the layout to compute the preferred size now that all sections
    // have been added (addSection() runs BEFORE show()). adjustSize() polls
    // sizeHint() and resizes the dialog to it.
    adjustSize();
    const QSize natural = sizeHint();
    // Width: take the content's preferred width, clamped to a sensible
    // legibility range. Old cap of 460 was too tight for monospace value
    // columns ("Mean = 0.014592503") and forced wrapping that made the
    // sheet look cramped. 620 is comfortable on a 1080p+ screen without
    // dominating the viewer.
    const int parentQuarter = parentGeo.width() / 4;
    const int width = qBound(420,
                             qMax(natural.width(), parentQuarter),
                             qMax(540, qMin(parentGeo.width() / 3, 720)));
    // Height: also content-driven. Cap at parent height — the inner
    // QScrollArea takes over if the content genuinely doesn't fit.
    const int height = qBound(220, natural.height(), parentGeo.height());

    int x = parentGeo.right() - width + 1;
    int y = parentGeo.top();

    // Keep the sheet on-screen if the parent is right-aligned to the display.
    if (auto *scr = root->screen()) {
        const QRect avail = scr->availableGeometry();
        x = qMin(x, avail.right() - width);
        y = qMax(y, avail.top());
    }

    setGeometry(x, y, width, height);
    m_anchored = true;
}

void ToolResultDialog::setTitle(const QString &title)
{
    if (m_titleLabel) {
        m_titleLabel->setText(title);
        setWindowTitle(title);
    }
}

void ToolResultDialog::setSubtitle(const QString &subtitle)
{
    if (m_subtitleLabel) {
        m_subtitleLabel->setText(subtitle);
        m_subtitleLabel->setVisible(!subtitle.isEmpty());
    }
}

void ToolResultDialog::addSection(const QString &sectionTitle,
                                  const QList<QPair<QString, QString>> &rows)
{
    if (!m_bodyLayout || rows.isEmpty())
        return;

    // Insert before the trailing stretch.
    const int insertIdx = m_bodyLayout->count() - 1;

    auto *box = new QGroupBox(sectionTitle);
    box->setStyleSheet(
            u"QGroupBox {"
            u"  background: transparent;"
            u"  border: 1px solid %1;"
            u"  border-radius: 6px;"
            u"  margin-top: 10px;"
            u"  color: %2;"
            u"  font-weight: 700;"
            u"  font-size: 10px;"
            u"  letter-spacing: 0.10em;"
            u"}"
            u"QGroupBox::title {"
            u"  subcontrol-origin: margin;"
            u"  left: 10px;"
            u"  padding: 0 6px;"
            u"  text-transform: uppercase;"
            u"}"_s.arg(tBorder(), tGroupTitle()));

    auto *form = new QFormLayout(box);
    form->setContentsMargins(12, 14, 12, 10);
    form->setHorizontalSpacing(24);
    form->setVerticalSpacing(6);
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);

    for (const auto &row : rows) {
        auto *keyLabel = new QLabel(row.first + u":"_s);
        keyLabel->setStyleSheet(
                u"color: %1; font-size: 11px; background: transparent;"_s.arg(tKey()));

        auto *valLabel = new QLabel(row.second);
        valLabel->setStyleSheet(
                u"color: %1; font-size: 11px; background: transparent; "
                u"font-family: 'JetBrains Mono', 'SF Mono', Menlo, monospace;"_s
                        .arg(tValue()));
        valLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);

        form->addRow(keyLabel, valLabel);
    }

    m_bodyLayout->insertWidget(insertIdx, box);
}
