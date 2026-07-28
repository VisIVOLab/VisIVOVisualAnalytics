#include "InspectorPanel.h"

#include "theme/VisivoTheme.h"

#include <QAction>
#include <QButtonGroup>
#include <QFileDialog>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QTabWidget>
#include <QToolButton>
#include <QVBoxLayout>

using namespace Qt::StringLiterals;

namespace {

QLabel *sectionCaption(const QString &text, QWidget *parent)
{
    auto *lbl = new QLabel(text.toUpper(), parent);
    lbl->setStyleSheet(
            QStringLiteral("QLabel { color: %1; font-size: 10px; font-weight: 700;"
                           " letter-spacing: 0.12em; padding: 6px 0 2px 0; }")
                    .arg(QLatin1StringView(VisivoTheme::kOnSurfaceVariant())));
    return lbl;
}

QToolButton *paneBtn(const QString &glyph, QWidget *parent)
{
    auto *b = new QToolButton(parent);
    b->setText(glyph);
    b->setCheckable(true);
    b->setCursor(Qt::PointingHandCursor);
    b->setStyleSheet(
            QStringLiteral(
                    "QToolButton { background:%1; color:%2; border:1px solid %3;"
                    " border-radius:5px; padding:2px 8px; font-size:11px; font-weight:600; }"
                    "QToolButton:checked { color:%4; border-color:%4; }"
                    "QToolButton:disabled { color:%3; }")
                    .arg(QLatin1StringView(VisivoTheme::kSurfaceContainerHigh()),
                         QLatin1StringView(VisivoTheme::kOnSurfaceVariant()),
                         QLatin1StringView(VisivoTheme::kOutline()),
                         QLatin1StringView(VisivoTheme::kPrimary())));
    return b;
}

} // namespace

InspectorPanel::InspectorPanel(QWidget *parent)
    : QWidget(parent)
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(8);

    // ── Pane-layout / Linked strip ───────────────────────────────────────────
    // (The backend/host chip lives once in the command bar — not duplicated here.)
    auto *topRow = new QHBoxLayout;
    topRow->setSpacing(6);
    topRow->addStretch(1);
    m_pane1 = paneBtn(u"▪"_s, this);
    m_pane2 = paneBtn(u"▪▪"_s, this);
    m_pane4 = paneBtn(u"⊞"_s, this);
    m_pane1->setToolTip(u"Single pane"_s);
    m_pane2->setToolTip(u"Two panes"_s);
    m_pane4->setToolTip(u"Four panes"_s);
    connect(m_pane1, &QToolButton::clicked, this, [this]() { emit paneLayoutRequested(1); });
    connect(m_pane2, &QToolButton::clicked, this, [this]() { emit paneLayoutRequested(2); });
    connect(m_pane4, &QToolButton::clicked, this, [this]() { emit paneLayoutRequested(4); });
    // Exclusive: exactly one pane-layout is "current".
    auto *paneGroup = new QButtonGroup(this);
    paneGroup->setExclusive(true);
    paneGroup->addButton(m_pane1);
    paneGroup->addButton(m_pane2);
    paneGroup->addButton(m_pane4);
    m_pane2->setChecked(true);   // two-pane is the default workspace layout
    topRow->addWidget(m_pane1);
    topRow->addWidget(m_pane2);
    topRow->addWidget(m_pane4);
    m_linkedBtn = paneBtn(u"⛓ Linked"_s, this);
    m_linkedBtn->setToolTip(u"Sync camera · channel · LUT across linked viewers"_s);
    connect(m_linkedBtn, &QToolButton::toggled, this,
            [this](bool on) { emit linkedToggled(on); });
    topRow->addWidget(m_linkedBtn);
    root->addLayout(topRow);

    // ── Tabs ──────────────────────────────────────────────────────────────────
    m_tabs = new QTabWidget(this);
    root->addWidget(m_tabs, 1);

    // Properties (scrollable — hosts KV rows plus migrated setting sections)
    auto *propsScroll = new QScrollArea(m_tabs);
    propsScroll->setWidgetResizable(true);
    propsScroll->setFrameShape(QFrame::NoFrame);
    m_propsPage = new QWidget(propsScroll);
    m_propsLayout = new QVBoxLayout(m_propsPage);
    m_propsLayout->setContentsMargins(4, 8, 4, 4);
    m_propsLayout->setSpacing(4);
    m_propsLayout->addStretch(1);
    propsScroll->setWidget(m_propsPage);
    m_tabs->addTab(propsScroll, u"Properties"_s);

    // Analysis (scrollable list of tool groups)
    auto *analysisScroll = new QScrollArea(m_tabs);
    analysisScroll->setWidgetResizable(true);
    analysisScroll->setFrameShape(QFrame::NoFrame);
    m_analysisPage = new QWidget(analysisScroll);
    m_analysisLayout = new QVBoxLayout(m_analysisPage);
    m_analysisLayout->setContentsMargins(4, 8, 4, 4);
    m_analysisLayout->setSpacing(2);
    m_analysisLayout->addStretch(1);
    analysisScroll->setWidget(m_analysisPage);
    m_tabs->addTab(analysisScroll, u"Analysis"_s);

    // Provenance
    m_provPage = new QWidget(m_tabs);
    m_provLayout = new QVBoxLayout(m_provPage);
    m_provLayout->setContentsMargins(4, 8, 4, 4);
    m_provLayout->setSpacing(6);
    m_provTitle = new QLabel(u"Select a product in Session Data."_s, m_provPage);
    m_provTitle->setWordWrap(true);
    m_provTitle->setStyleSheet(
            QStringLiteral("QLabel { color:%1; font-weight:600; }")
                    .arg(QLatin1StringView(VisivoTheme::kOnSurface())));
    m_provBody = new QLabel(m_provPage);
    m_provBody->setWordWrap(true);
    m_provBody->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_provBody->setStyleSheet(
            QStringLiteral("QLabel { color:%1; font-family:'JetBrains Mono',monospace;"
                           " font-size:11px; }")
                    .arg(QLatin1StringView(VisivoTheme::kOnSurfaceVariant())));
    auto *exportBtn = new QPushButton(u"Export recipe as JSON…"_s, m_provPage);
    exportBtn->setCursor(Qt::PointingHandCursor);
    connect(exportBtn, &QPushButton::clicked, this, [this]() {
        if (m_provParams.isEmpty())
            return;
        const QString path = QFileDialog::getSaveFileName(
                this, u"Export recipe"_s, u"recipe.json"_s, u"JSON (*.json)"_s);
        if (path.isEmpty())
            return;
        QFile f(path);
        if (f.open(QIODevice::WriteOnly)) {
            f.write(QJsonDocument(QJsonObject::fromVariantMap(m_provParams)).toJson());
        }
    });
    m_provLayout->addWidget(m_provTitle);
    m_provLayout->addWidget(m_provBody);
    m_provLayout->addWidget(exportBtn);
    m_provLayout->addStretch(1);
    m_tabs->addTab(m_provPage, u"Provenance"_s);
}

void InspectorPanel::setProperties(const QList<QPair<QString, QString>> &rows)
{
    // Clear existing rows (keep the trailing stretch).
    while (m_propsLayout->count() > 1) {
        QLayoutItem *item = m_propsLayout->takeAt(0);
        if (QWidget *w = item->widget())
            w->deleteLater();
        delete item;
    }
    int insertAt = 0;
    for (const auto &kv : rows) {
        auto *row = new QLabel(m_propsPage);
        row->setTextFormat(Qt::RichText);
        row->setText(QStringLiteral("<span style='color:%1'>%2</span>&nbsp;&nbsp;"
                                    "<span style='color:%3'>%4</span>")
                             .arg(QLatin1StringView(VisivoTheme::kOnSurfaceVariant()),
                                  kv.first.toHtmlEscaped(),
                                  QLatin1StringView(VisivoTheme::kOnSurface()),
                                  kv.second.toHtmlEscaped()));
        row->setTextInteractionFlags(Qt::TextSelectableByMouse);
        m_propsLayout->insertWidget(insertAt++, row);
    }
}

void InspectorPanel::addPropertiesSection(QWidget *section)
{
    if (!section)
        return;
    section->setParent(m_propsPage);
    // Insert before the trailing stretch so sections stack under the KV rows.
    m_propsLayout->insertWidget(m_propsLayout->count() - 1, section);
    section->show();
}

QVBoxLayout *InspectorPanel::groupLayout(const QString &caption)
{
    if (auto *l = m_groupLayouts.value(caption, nullptr))
        return l;
    // Insert a new group before the trailing stretch.
    auto *cap = sectionCaption(caption, m_analysisPage);
    m_analysisLayout->insertWidget(m_analysisLayout->count() - 1, cap);
    auto *box = new QWidget(m_analysisPage);
    auto *boxLayout = new QVBoxLayout(box);
    boxLayout->setContentsMargins(0, 0, 0, 4);
    boxLayout->setSpacing(2);
    m_analysisLayout->insertWidget(m_analysisLayout->count() - 1, box);
    m_groupLayouts.insert(caption, boxLayout);
    return boxLayout;
}

void InspectorPanel::addToolGroup(const QString &caption)
{
    groupLayout(caption);
}

void InspectorPanel::addTool(const QString &groupCaption, const QString &label, QAction *action)
{
    QVBoxLayout *g = groupLayout(groupCaption);
    auto *btn = new QToolButton(m_analysisPage);
    btn->setText(label);
    btn->setToolButtonStyle(Qt::ToolButtonTextOnly);
    btn->setCursor(Qt::PointingHandCursor);
    // Full-width + left-aligned text (matches the section headers above), same
    // treatment as the sidebar tool buttons.
    btn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    btn->setStyleSheet(VisivoTheme::sidebarToolButtonStyleSheet());
    if (action) {
        btn->setEnabled(action->isEnabled());
        connect(btn, &QToolButton::clicked, action, &QAction::trigger);
        connect(action, &QAction::changed, btn,
                [btn, action]() { btn->setEnabled(action->isEnabled()); });
    }
    g->addWidget(btn);
}

void InspectorPanel::showProvenance(const QString &title, const QVariantMap &params)
{
    m_provParams = params;
    m_provTitle->setText(title);
    if (params.isEmpty()) {
        m_provBody->setText(u"No recorded parameters for this product."_s);
        return;
    }
    QStringList lines;
    for (auto it = params.constBegin(); it != params.constEnd(); ++it)
        lines << QStringLiteral("%1 = %2").arg(it.key(), it.value().toString());
    m_provBody->setText(lines.join(u'\n'));
}

void InspectorPanel::clearProvenance()
{
    m_provParams.clear();
    m_provTitle->setText(u"Select a product in Session Data."_s);
    m_provBody->clear();
}

void InspectorPanel::setPaneLayoutEnabled(bool enabled)
{
    m_pane1->setEnabled(enabled);
    m_pane2->setEnabled(enabled);
    m_pane4->setEnabled(enabled);
}

void InspectorPanel::setLinkedSupported(bool supported)
{
    m_linkedBtn->setEnabled(supported);
    m_linkedBtn->setVisible(supported);
}

void InspectorPanel::setLinkedChecked(bool checked)
{
    QSignalBlocker b(m_linkedBtn);
    m_linkedBtn->setChecked(checked);
}
