#include "ToolResultDialog.h"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QScrollArea>
#include <QVBoxLayout>

using namespace Qt::StringLiterals;

// Palette constants matching the VisIVO cube viewer sidebar theme.
static const QString kDlgBg         = u"#141922"_s;
static const QString kHeaderBg      = u"#1e2738"_s;
static const QString kHeaderBorder  = u"#2a3a5a"_s;
static const QString kSectionBorder = u"rgba(4, 138, 191, 0.25)"_s;
static const QString kKeyColor      = u"#7888a8"_s;
static const QString kValueColor    = u"#d0deff"_s;
static const QString kTitleColor    = u"#e0eaff"_s;
static const QString kSubtitleColor = u"#8a9abf"_s;
static const QString kGroupColor    = u"rgba(4, 138, 191, 0.80)"_s;

ToolResultDialog::ToolResultDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(u"Analysis Result"_s);
    setMinimumWidth(440);
    setSizeGripEnabled(true);

    auto *outerLayout = new QVBoxLayout(this);
    outerLayout->setSpacing(0);
    outerLayout->setContentsMargins(0, 0, 0, 0);

    // ── Header ───────────────────────────────────────────────────────────────
    auto *headerWidget = new QWidget();
    headerWidget->setObjectName(u"toolResultHeader"_s);
    headerWidget->setStyleSheet(
            u"QWidget#toolResultHeader {"
            "  background: %1;"
            "  border-bottom: 1px solid %2;"
            "}"_s.arg(kHeaderBg, kHeaderBorder));

    auto *headerLayout = new QVBoxLayout(headerWidget);
    headerLayout->setContentsMargins(16, 12, 16, 12);
    headerLayout->setSpacing(3);

    m_titleLabel = new QLabel();
    m_titleLabel->setStyleSheet(
            u"font-size: 14px; font-weight: bold; color: %1;"_s.arg(kTitleColor));
    headerLayout->addWidget(m_titleLabel);

    m_subtitleLabel = new QLabel();
    m_subtitleLabel->setStyleSheet(
            u"font-size: 11px; color: %1;"_s.arg(kSubtitleColor));
    m_subtitleLabel->setWordWrap(true);
    m_subtitleLabel->hide();
    headerLayout->addWidget(m_subtitleLabel);

    outerLayout->addWidget(headerWidget);

    // ── Scrollable body ───────────────────────────────────────────────────────
    auto *scrollArea = new QScrollArea();
    scrollArea->setFrameShape(QFrame::NoFrame);
    scrollArea->setWidgetResizable(true);
    scrollArea->setStyleSheet(u"QScrollArea { background: %1; }"_s.arg(kDlgBg));

    auto *bodyWidget = new QWidget();
    bodyWidget->setStyleSheet(u"background: %1;"_s.arg(kDlgBg));
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
                    .arg(kHeaderBg, kHeaderBorder));
    auto *footerLayout = new QHBoxLayout(footer);
    footerLayout->setContentsMargins(12, 8, 12, 8);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    QObject::connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::accept);
    footerLayout->addWidget(buttons);

    outerLayout->addWidget(footer);
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
            "  border: 1px solid %1;"
            "  border-radius: 5px;"
            "  margin-top: 8px;"
            "  color: %2;"
            "  font-weight: bold;"
            "  font-size: 11px;"
            "}"
            "QGroupBox::title {"
            "  subcontrol-origin: margin;"
            "  left: 8px;"
            "  padding: 0 4px;"
            "}"_s.arg(kSectionBorder, kGroupColor));

    auto *form = new QFormLayout(box);
    form->setContentsMargins(12, 14, 12, 10);
    form->setHorizontalSpacing(24);
    form->setVerticalSpacing(6);
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);

    for (const auto &row : rows) {
        auto *keyLabel = new QLabel(row.first + u":"_s);
        keyLabel->setStyleSheet(
                u"color: %1; font-size: 11px;"_s.arg(kKeyColor));

        auto *valLabel = new QLabel(row.second);
        valLabel->setStyleSheet(
                u"color: %1; font-size: 11px;"_s.arg(kValueColor));
        valLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);

        form->addRow(keyLabel, valLabel);
    }

    m_bodyLayout->insertWidget(insertIdx, box);
}
