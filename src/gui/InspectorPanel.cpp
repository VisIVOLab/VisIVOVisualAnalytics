#include "InspectorPanel.h"

#include <QPointer>
#include <QSignalBlocker>

#include <functional>

#include "AssistantPanel.h"
#include "app/ProductRecipe.h"
#include "theme/VisivoTheme.h"

#include <QAction>
#include <QButtonGroup>
#include <QClipboard>
#include <QCoreApplication>
#include <QDateTime>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMenu>
#include <QDebug>
#include <QFileDialog>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
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

    // Provenance (scrollable: a tool's settings block plus a long recipe must
    // not squeeze each other into the panel's height)
    auto *provScroll = new QScrollArea(m_tabs);
    provScroll->setWidgetResizable(true);
    provScroll->setFrameShape(QFrame::NoFrame);
    provScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_provPage = new QWidget(provScroll);
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
    // showProvenance() puts zero-width spaces into long values so they wrap; a
    // copy out of this label must not carry them into a path or a number, so
    // both ways to copy (the shortcut and the context menu) go through here.
    m_provBody->installEventFilter(this);
    m_provBody->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_provBody, &QLabel::customContextMenuRequested, this, [this](const QPoint &pos) {
        QMenu menu(m_provBody);
        QAction *copy = menu.addAction(u"Copy"_s);
        copy->setShortcut(QKeySequence::Copy);
        copy->setEnabled(m_provBody->hasSelectedText());
        QAction *chosen = menu.exec(m_provBody->mapToGlobal(pos));
        if (chosen == copy)
            copyProvenanceSelection();
    });
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
            f.write(QJsonDocument(recipeJson(m_provProduct)).toJson());
        }
    });
    // ── Two halves, one tab ──────────────────────────────────────────────────
    // The tool you have OPEN and the product you have SELECTED both answer the
    // same question - "which numbers produced what I am looking at" - so they
    // live in one place instead of the three the settings used to be spread
    // over (a dock block, a dialog, nowhere). The top half is the active tool's
    // live settings, the bottom half the recorded ones of the selected product.
    // Nothing is lost by merging: the bottom half IS the provenance - still
    // called that, still with its Export recipe button - it just no longer has
    // the tab to itself.
    m_toolCaption = new QLabel(m_provPage);
    m_toolCaption->setStyleSheet(
            QStringLiteral("QLabel { color:%1; font-weight:700; font-size:10px;"
                           " letter-spacing:0.14em; }")
                    .arg(QLatin1StringView(VisivoTheme::kOnSurfaceVariant())));
    m_toolCaption->hide();
    m_toolSettingsHost = new QWidget(m_provPage);
    auto *toolHostLayout = new QVBoxLayout(m_toolSettingsHost);
    toolHostLayout->setContentsMargins(0, 0, 0, 0);
    toolHostLayout->setSpacing(4);
    m_toolSettingsHost->hide();
    // Where a settings block waits while its tool is off. Unmounting used to
    // setParent(nullptr), which does not delete the widget but takes it out of
    // every ownership tree: nothing freed it and nothing would have. Parked here
    // it is owned by this panel, hidden, and ready to be mounted again.
    m_toolSettingsParking = new QWidget(m_provPage);
    m_toolSettingsParking->setObjectName(u"toolSettingsParking"_s);
    m_toolSettingsParking->hide();
    m_provCaption = new QLabel(u"PROVENANCE"_s, m_provPage);
    m_provCaption->setStyleSheet(m_toolCaption->styleSheet());

    m_provLayout->addWidget(m_toolCaption);
    m_provLayout->addWidget(m_toolSettingsHost);
    m_provLayout->addWidget(m_provCaption);
    m_provLayout->addWidget(m_provTitle);
    m_provLayout->addWidget(m_provBody);
    m_provLayout->addWidget(exportBtn);
    m_provLayout->addStretch(1);
    provScroll->setWidget(m_provPage);
    m_tabs->addTab(provScroll, u"Parameters"_s);
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

void InspectorPanel::applyValueStyle(QLabel *label, const QString &value)
{
    if (!label)
        return;
    label->setStyleSheet(
            QStringLiteral("color:%1; font-family:'JetBrains Mono','SF Mono',monospace;"
                           " font-size:11px;")
                    .arg(QLatin1StringView(VisivoTheme::kOnSurface())));
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    label->setToolTip(value);
    // A long value — a path or a session UUID — used to widen the whole
    // Properties page: the panel is a fixed 320 px, so the page then scrolled
    // sideways and every SHORT value was pushed out of sight beyond the right
    // edge. The rows looked empty. Elided with the label's own metrics, full
    // text in the tooltip.
    constexpr int kMaxValueWidth = 190;
    label->setMaximumWidth(kMaxValueWidth);
    label->ensurePolished();
    label->setText(label->fontMetrics().elidedText(value, Qt::ElideMiddle, kMaxValueWidth));
}

void InspectorPanel::addPropertyRow(const QString &key, const QString &value)
{
    auto *val = new QLabel;
    applyValueStyle(val, value);
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

void InspectorPanel::addAnalysisSection(QWidget *section)
{
    if (!section)
        return;
    section->setParent(m_analysisPage);
    m_analysisLayout->insertWidget(0, section);   // top of the tab, above the tool groups
}

void InspectorPanel::addToolGroup(const QString &caption)
{
    groupLayout(caption);
}

namespace {

/// A tool button that announces itself BEFORE its default action runs.
///
/// QAbstractButton emits clicked() only after nextCheckState() has already
/// triggered the action — so with a default action, a connection on clicked()
/// arrives after a modal dialog has opened AND closed. The Inspector needs to
/// expand onto its Analysis tab first (§4.1.4), which is the whole reason the
/// notification exists, so it has to go out from inside nextCheckState().
class ToolLaunchButton : public QToolButton
{
public:
    ToolLaunchButton(std::function<void()> announce, QWidget *parent)
        : QToolButton(parent), m_announce(std::move(announce))
    {
    }

protected:
    void nextCheckState() override
    {
        // Guarded: the announcement runs a slot synchronously, and if a future
        // one ever deletes this button the base call below would run on a dead
        // object. Qt's own guard wraps the click, not the inside of an override.
        QPointer<ToolLaunchButton> alive(this);
        if (m_announce)
            m_announce();
        if (!alive)
            return;
        QToolButton::nextCheckState();   // triggers the default action
    }

private:
    std::function<void()> m_announce;
};

}  // namespace

void InspectorPanel::addTool(const QString &groupCaption, const QString &label, QAction *action)
{
    QVBoxLayout *g = groupLayout(groupCaption);
    auto *btn = new ToolLaunchButton([this]() { emit toolLaunched(); }, m_analysisPage);
    btn->setText(label);
    btn->setToolButtonStyle(Qt::ToolButtonTextOnly);
    btn->setCursor(Qt::PointingHandCursor);
    // Full-width + left-aligned text (matches the section headers above), same
    // treatment as the sidebar tool buttons.
    btn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    btn->setStyleSheet(VisivoTheme::sidebarToolButtonStyleSheet());
    if (action) {
        // setDefaultAction, not a hand-rolled connect(changed).
        //
        // The button has to mirror enabled AND checked state now that this tab
        // carries every tool rather than a hand-picked set of dialogs — and a
        // manual connection to QAction::changed() misses every place that
        // updates an action under a QSignalBlocker, of which this codebase has
        // several: clearing a catalogue overlay, cancelling PV Width, the linked
        // blend mode. Qt delivers QEvent::ActionChanged to a default-action
        // button regardless of blocked SIGNALS, so this tracks all of them
        // without a patch at each site.
        //
        // The trade is that the action owns the text; `label` is honoured only
        // when there is no action to take it from.
        btn->setDefaultAction(action);
        // The announcement comes from ToolLaunchButton, before the trigger —
        // see the note there for why clicked() is too late.

        // A DISABLED tool must still be able to say why. Qt does not deliver
        // QEvent::ToolTip to a disabled widget, so the gate's carefully written
        // reasons ("open a 2D image viewer first - the slice overlays there as
        // contours") were invisible exactly when they were needed, and a greyed
        // entry in this list was mute (user report: "why isn't Send Slice to
        // Image Viewer clickable?"). Mouse events over a disabled child go to
        // its parent, so the reason is mirrored onto a wrapper that stays
        // enabled, and kept in step with the action.
        auto *slot = new QWidget(m_analysisPage);
        auto *slotLayout = new QVBoxLayout(slot);
        slotLayout->setContentsMargins(0, 0, 0, 0);
        slotLayout->setSpacing(0);
        slotLayout->addWidget(btn);
        const auto mirrorTip = [slot, action]() {
            slot->setToolTip(action->toolTip());
        };
        mirrorTip();
        QObject::connect(action, &QAction::changed, slot, mirrorTip);
        g->addWidget(slot);
        return;
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

bool InspectorPanel::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_provBody && event->type() == QEvent::ShortcutOverride
        && static_cast<QKeyEvent *>(event)->matches(QKeySequence::Copy)) {
        event->accept();   // keep the shortcut ours, then the key press arrives
        return true;
    }
    if (watched == m_provBody && event->type() == QEvent::KeyPress
        && static_cast<QKeyEvent *>(event)->matches(QKeySequence::Copy)) {
        copyProvenanceSelection();
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

void InspectorPanel::copyProvenanceSelection() const
{
    if (m_provBody->hasSelectedText())
        QGuiApplication::clipboard()->setText(m_provBody->selectedText().remove(QChar(0x200B)));
}

QJsonObject InspectorPanel::recipeJson(const AnalysisProduct &p)
{
    // The record itself is ProductRecipe (testable without a widget); the panel
    // only says who is writing it and when.
    return visivo::productRecipe(
            p,
            u"%1 %2"_s.arg(QCoreApplication::applicationName(),
                           QCoreApplication::applicationVersion()),
            QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
}

void InspectorPanel::showProvenance(const AnalysisProduct &product)
{
    showProvenance(product.label.isEmpty() ? product.kind : product.label, product.params);
    m_provProduct = product;   // after: the two-argument form resets it
}

void InspectorPanel::showProvenance(const QString &title, const QVariantMap &params)
{
    m_provParams = params;
    m_provProduct = AnalysisProduct{};
    m_provProduct.label = title;
    m_provProduct.params = params;
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
    // Values such as file names and seed lists are long unbroken tokens: a
    // zero-width space after each separator lets them wrap inside the panel
    // instead of widening the table past its right edge.
    const auto breakable = [](const QString &value) {
        QString out = value.toHtmlEscaped();
        for (const QString &sep : { u"_"_s, u"/"_s, u","_s, u";"_s, u"="_s, u"-"_s, u"."_s })
            out.replace(sep, sep + QChar(0x200B));
        return out;
    };
    QString html = u"<table width='100%' cellspacing='0' cellpadding='2'>"_s;
    for (const QString &k : std::as_const(keys)) {
        html += QStringLiteral(
                        "<tr><td valign='top' style='color:%1; font-family:Lato; font-size:11px;"
                        " white-space:nowrap;'>%2</td>"
                        "<td align='right' style='color:%3;"
                        " font-family:\"JetBrains Mono\",\"SF Mono\",monospace; font-size:11px;'>%4</td></tr>")
                        .arg(muted, k.toHtmlEscaped(), ink, breakable(params.value(k).toString()));
    }
    html += u"</table>"_s;
    m_provBody->setText(html);
    m_provBody->setTextFormat(Qt::RichText);
}

QString InspectorPanel::activeToolSettings() const
{
    return m_activeToolName;
}

void InspectorPanel::setToolSettings(const QString &toolName, QWidget *settings)
{
    if (!m_toolSettingsHost)
        return;
    auto *layout = qobject_cast<QVBoxLayout *>(m_toolSettingsHost->layout());
    if (!layout)
        return;
    // Re-home whatever was there: the widget belongs to the window, which keeps
    // it alive and reuses it, so it is removed from this layout, never deleted.
    while (QLayoutItem *item = layout->takeAt(0)) {
        if (QWidget *w = item->widget()) {
            w->setParent(m_toolSettingsParking);
            w->hide();
        }
        delete item;
    }
    if (!settings) {
        clearToolSettings();
        return;
    }
    layout->addWidget(settings);
    settings->show();
    m_activeToolName = toolName;
    m_toolCaption->setText(toolName.toUpper());
    m_toolCaption->show();
    m_toolSettingsHost->show();
}

void InspectorPanel::clearToolSettings()
{
    m_activeToolName.clear();
    if (!m_toolSettingsHost)
        return;
    if (auto *layout = qobject_cast<QVBoxLayout *>(m_toolSettingsHost->layout())) {
        while (QLayoutItem *item = layout->takeAt(0)) {
            if (QWidget *w = item->widget()) {
                w->setParent(m_toolSettingsParking);
                w->hide();
            }
            delete item;
        }
    }
    m_toolSettingsHost->hide();
    if (m_toolCaption)
        m_toolCaption->hide();
}

void InspectorPanel::clearProvenance()
{
    m_provParams.clear();
    m_provProduct = AnalysisProduct{};
    m_provTitle->setText(u"Select a product in Session Data."_s);
    m_provBody->clear();
}

AssistantPanel *InspectorPanel::ensureCopilotTab()
{
    if (!m_copilot) {
        m_copilot = new AssistantPanel(this);
        m_tabs->addTab(m_copilot, u"Copilot"_s);
    }
    return m_copilot;
}

