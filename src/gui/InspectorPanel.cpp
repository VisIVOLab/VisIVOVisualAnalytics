#include "InspectorPanel.h"

#include "theme/VisivoTheme.h"

#include <QAction>
#include <QButtonGroup>
#include <QDebug>
#include <QFileDialog>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
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


} // namespace

InspectorPanel::InspectorPanel(QWidget *parent)
    : QWidget(parent)
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(8);

    // NOTE: the 1/2/4 + Linked strip lives in the CENTRE toolbar (above the pane
    // splitter), not here — it controls the views, not the panel. Keeping it out
    // of the Inspector also gives the three tabs the full 320px (no truncation).

    // ── Tabs ──────────────────────────────────────────────────────────────────
    m_tabs = new QTabWidget(this);
    // Three short labels fit 320px easily — never elide them; and trim the tab
    // padding (the global QSS default was wide enough to force truncation).
    m_tabs->tabBar()->setElideMode(Qt::ElideNone);
    m_tabs->tabBar()->setExpanding(false);
    m_tabs->setStyleSheet(QStringLiteral("QTabBar::tab { padding: 8px 12px; }"));
    root->addWidget(m_tabs, 1);

    // Properties (scrollable — hosts KV rows plus migrated setting sections)
    auto *propsScroll = new QScrollArea(m_tabs);
    m_propsScroll = propsScroll;
    propsScroll->setWidgetResizable(true);
    propsScroll->setFrameShape(QFrame::NoFrame);
    m_propsPage = new QWidget(propsScroll);
    m_propsPage->setObjectName(u"inspectorPropertiesPage"_s);
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

void InspectorPanel::clearProperties()
{
    while (m_propsLayout->count() > 1) {   // keep the trailing stretch
        QLayoutItem *item = m_propsLayout->takeAt(0);
        if (QWidget *w = item->widget())
            w->deleteLater();
        delete item;
    }
}

void InspectorPanel::addPropertyCaption(const QString &caption, bool sub)
{
    auto *lbl = new QLabel(caption, m_propsPage);
    if (sub) {
        lbl->setStyleSheet(
                QStringLiteral("QLabel { color:%1; font-family:'Lato'; font-size:10px;"
                               " font-weight:600; padding:6px 0 1px 0; }")
                        .arg(QLatin1StringView(VisivoTheme::kSecondary())));
    } else {
        lbl->setStyleSheet(
                QStringLiteral("QLabel { color:%1; %2 padding:10px 0 3px 0; }")
                        .arg(QLatin1StringView(VisivoTheme::kOnSurfaceVariant()),
                             VisivoTheme::sectionHeaderCss()));
    }
    m_propsLayout->insertWidget(m_propsLayout->count() - 1, lbl);
}

void InspectorPanel::addPropertyRow(const QString &key, const QString &value)
{
    auto *val = new QLabel(value);
    val->setStyleSheet(
            QStringLiteral("color:%1; font-family:'JetBrains Mono','SF Mono',monospace;"
                           " font-size:11px;")
                    .arg(QLatin1StringView(VisivoTheme::kOnSurface())));
    val->setTextInteractionFlags(Qt::TextSelectableByMouse);
    val->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    val->setToolTip(value);
    addPropertyRow(key, static_cast<QWidget *>(val));
}

void InspectorPanel::addPropertyRow(const QString &key, QWidget *valueWidget)
{
    auto *row = new QWidget(m_propsPage);
    auto *h = new QHBoxLayout(row);
    h->setContentsMargins(0, 1, 0, 1);
    h->setSpacing(8);
    auto *k = new QLabel(key, row);
    k->setStyleSheet(QStringLiteral("color:%1; font-family:'Lato'; font-size:11px;")
                             .arg(QLatin1StringView(VisivoTheme::kOnSurfaceVariant())));
    h->addWidget(k, 1);
    if (valueWidget) {
        // Detach from any previous layout first (re-hosted live widgets), so no
        // stale layout item is left behind in the parked info page.
        if (auto *p = valueWidget->parentWidget())
            if (auto *l = p->layout())
                l->removeWidget(valueWidget);
        valueWidget->setParent(row);
        h->addWidget(valueWidget, 0, Qt::AlignRight);
    }
    m_propsLayout->insertWidget(m_propsLayout->count() - 1, row);
}

void InspectorPanel::setProperties(const QList<QPair<QString, QString>> &rows)
{
    clearProperties();
    for (const auto &kv : rows)
        addPropertyRow(kv.first, kv.second);
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
        // Emit BEFORE triggering the action: the tool may open a modal dialog, and
        // we want the Inspector to expand onto Analysis first (§4.1.4).
        connect(btn, &QToolButton::clicked, this, &InspectorPanel::toolLaunched);
        connect(btn, &QToolButton::clicked, action, &QAction::trigger);
        connect(action, &QAction::changed, btn,
                [btn, action]() { btn->setEnabled(action->isEnabled()); });
    }
    g->addWidget(btn);
}

void InspectorPanel::showTab(int index)
{
    if (m_tabs && index >= 0 && index < m_tabs->count())
        m_tabs->setCurrentIndex(index);
}

int InspectorPanel::currentTab() const
{
    return m_tabs ? m_tabs->currentIndex() : -1;
}

int InspectorPanel::propertiesContentHeight() const
{
    return m_propsPage ? m_propsPage->sizeHint().height() : 0;
}

void InspectorPanel::logPropertiesFit()
{
    // Defer to after the first layout pass — at construction the scroll viewport
    // has no real height yet (reads ~30 px), which is meaningless.
    QTimer::singleShot(0, this, [this]() {
        if (!m_propsPage || !m_propsScroll)
            return;
        const int content = m_propsPage->sizeHint().height();
        const int viewport = m_propsScroll->viewport()->height();
        const QRect g = m_propsPage->geometry();
        qDebug().noquote() << QStringLiteral(
                "[step0] Properties fit: widget='%1' geom=%2,%3 %4x%5  contentHint=%6  "
                "viewport=%7  %8")
                .arg(m_propsPage->objectName())
                .arg(g.x()).arg(g.y()).arg(g.width()).arg(g.height())
                .arg(content).arg(viewport)
                .arg(content <= viewport ? u"OK (no scroll)"_s
                                         : u"OVERFLOW → scrolls"_s);
    });
}

void InspectorPanel::showProvenance(const QString &title, const QVariantMap &params)
{
    m_provParams = params;
    m_provTitle->setText(title);
    if (params.isEmpty()) {
        m_provBody->setText(u"No recorded parameters for this product."_s);
        return;
    }
    // Same grid as Properties (§3.0c): key left/muted, value right/mono. And a
    // preferred order — Order is the product's identity, so it leads; Channels/Mask
    // are its parameters. Unknown keys follow, in insertion/alpha order.
    QStringList keys;
    for (const QString &k : { u"Order"_s, u"Channels"_s, u"Mask"_s, u"Scope"_s, u"Unit"_s })
        if (params.contains(k))
            keys << k;
    for (auto it = params.constBegin(); it != params.constEnd(); ++it)
        if (!keys.contains(it.key()))
            keys << it.key();

    const QString muted = QString::fromLatin1(VisivoTheme::kOnSurfaceVariant());
    const QString ink = QString::fromLatin1(VisivoTheme::kOnSurface());
    QString html = u"<table width='100%' cellspacing='0' cellpadding='2'>"_s;
    for (const QString &k : std::as_const(keys)) {
        html += QStringLiteral(
                        "<tr><td style='color:%1; font-family:Lato; font-size:11px;'>%2</td>"
                        "<td align='right' style='color:%3;"
                        " font-family:\"JetBrains Mono\",\"SF Mono\",monospace; font-size:11px;'>%4</td></tr>")
                        .arg(muted, k.toHtmlEscaped(), ink, params.value(k).toString().toHtmlEscaped());
    }
    html += u"</table>"_s;
    m_provBody->setText(html);
    m_provBody->setTextFormat(Qt::RichText);
}

void InspectorPanel::clearProvenance()
{
    m_provParams.clear();
    m_provTitle->setText(u"Select a product in Session Data."_s);
    m_provBody->clear();
}

