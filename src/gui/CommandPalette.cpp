#include "CommandPalette.h"
#include "theme/VisivoTheme.h"

#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QScreen>
#include <QShowEvent>
#include <QVBoxLayout>
#include <QWindow>

#include <algorithm>
#include <unordered_map>

using namespace Qt::StringLiterals;

namespace {

// Item types: distinguishes group separators (non-selectable) from real rows.
constexpr int kItemTypeAction = QListWidgetItem::UserType + 1;
constexpr int kItemTypeGroup  = QListWidgetItem::UserType + 2;

// QListWidgetItem data slots for action lookup.
constexpr int kActionIndexRole = Qt::UserRole + 1;

QString tBg()       { return QString::fromLatin1(VisivoTheme::kSurfaceContainerLow()); }
QString tInputBg()  { return QString::fromLatin1(VisivoTheme::kSurface()); }
QString tBorder()   { return QString::fromLatin1(VisivoTheme::kOutline()); }
QString tFg()       { return QString::fromLatin1(VisivoTheme::kOnSurface()); }
QString tFgDim()    { return QString::fromLatin1(VisivoTheme::kOnSurfaceVariant()); }
QString tFgMuted()  { return QString::fromLatin1(VisivoTheme::kSecondary()); }
QString tHover()    { return QString::fromLatin1(VisivoTheme::kSurfaceContainerHigh()); }
QString tSelected() { return QString::fromLatin1(VisivoTheme::kSurfaceContainerHighest()); }

} // namespace

CommandPalette::CommandPalette(QWidget *parent)
    : QDialog(parent,
              Qt::Dialog | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint)
{
    setAttribute(Qt::WA_TranslucentBackground, false);
    setModal(true);

    // ── Outer container ──────────────────────────────────────────────────────
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    setStyleSheet(
            QStringLiteral(
                    "CommandPalette {"
                    "  background-color: %1;"
                    "  border: 1px solid %2;"
                    "  border-radius: 10px;"
                    "}")
                    .arg(tBg(), tBorder()));

    // ── Search input row (icon + line-edit + esc hint) ───────────────────────
    auto *inputWrap = new QWidget(this);
    inputWrap->setObjectName(u"cmdInputWrap"_s);
    inputWrap->setStyleSheet(
            QStringLiteral(
                    "QWidget#cmdInputWrap {"
                    "  background-color: %1;"
                    "  border-bottom: 1px solid %2;"
                    "  border-top-left-radius: 10px;"
                    "  border-top-right-radius: 10px;"
                    "}"
                    "QWidget#cmdInputWrap QLabel { background: transparent; }")
                    .arg(tInputBg(), tBorder()));
    auto *inputLayout = new QHBoxLayout(inputWrap);
    inputLayout->setContentsMargins(16, 12, 14, 12);
    inputLayout->setSpacing(10);

    auto *searchIcon = new QLabel(u"⌕"_s, inputWrap);
    searchIcon->setStyleSheet(
            QStringLiteral("color: %1; font-size: 16px;").arg(tFgDim()));
    inputLayout->addWidget(searchIcon);

    m_input = new QLineEdit(inputWrap);
    m_input->setPlaceholderText(u"Search actions, datasets, layers…"_s);
    m_input->setFrame(false);
    m_input->setStyleSheet(
            QStringLiteral(
                    "QLineEdit {"
                    "  background: transparent;"
                    "  color: %1;"
                    "  font-size: 14px;"
                    "  border: none; padding: 0;"
                    "  selection-background-color: %2;"
                    "  selection-color: %3;"
                    "}")
                    .arg(tFg(),
                         QString::fromLatin1(VisivoTheme::kPrimary()),
                         QString::fromLatin1(VisivoTheme::kOnPrimary())));
    inputLayout->addWidget(m_input, 1);

    auto *escHint = new QLabel(u"esc"_s, inputWrap);
    escHint->setStyleSheet(
            QStringLiteral(
                    "background: %1; color: %2;"
                    "font-family: 'JetBrains Mono', 'SF Mono', Menlo, monospace;"
                    "font-size: 10px; padding: 2px 6px; border-radius: 3px;"
                    "border: 1px solid %3;")
                    .arg(QString::fromLatin1(VisivoTheme::kSurfaceContainerHigh()),
                         tFgMuted(), tBorder()));
    inputLayout->addWidget(escHint);
    outer->addWidget(inputWrap);

    // ── Results list ─────────────────────────────────────────────────────────
    m_list = new QListWidget(this);
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setFocusPolicy(Qt::NoFocus);  // keep keyboard on m_input
    m_list->setStyleSheet(
            QStringLiteral(
                    "QListWidget {"
                    "  background: transparent;"
                    "  border: none;"
                    "  outline: none;"
                    "  padding: 6px 0;"
                    "}"
                    "QListWidget::item { padding: 0; border: none; }"
                    "QListWidget::item:selected { background: %1; }"
                    "QListWidget::item:hover    { background: %2; }")
                    .arg(tSelected(), tHover()));
    outer->addWidget(m_list, 1);

    setFixedSize(620, 460);

    QObject::connect(m_input, &QLineEdit::textChanged,
                     this, &CommandPalette::onQueryChanged);
    QObject::connect(m_input, &QLineEdit::returnPressed, this, [this]() {
        if (auto *item = m_list->currentItem()) onItemActivated(item);
    });
    QObject::connect(m_list, &QListWidget::itemActivated,
                     this, &CommandPalette::onItemActivated);
    QObject::connect(m_list, &QListWidget::itemClicked,
                     this, &CommandPalette::onItemActivated);
}

void CommandPalette::addAction(Action action)
{
    m_actions.push_back(std::move(action));
}

void CommandPalette::clearGroup(const QString &group)
{
    m_actions.erase(
            std::remove_if(m_actions.begin(), m_actions.end(),
                           [&](const Action &a) { return a.group == group; }),
            m_actions.end());
}

void CommandPalette::showEvent(QShowEvent *event)
{
    QDialog::showEvent(event);

    // Reset filter every time the palette opens.
    if (m_input) m_input->clear();
    m_filter.clear();
    rebuildList();
    selectFirstSelectable();
    if (m_input) m_input->setFocus();

    // Center on the parent's top-level window (or the screen if parentless).
    if (auto *root = parentWidget() ? parentWidget()->window() : nullptr) {
        const QRect geo = root->geometry();
        move(geo.center().x() - width() / 2,
             geo.top() + qMax(80, geo.height() / 6));
    } else if (auto *scr = screen()) {
        const QRect avail = scr->availableGeometry();
        move(avail.center().x() - width() / 2,
             avail.top() + avail.height() / 6);
    }
}

void CommandPalette::keyPressEvent(QKeyEvent *event)
{
    switch (event->key()) {
    case Qt::Key_Escape:
        reject();
        return;
    case Qt::Key_Up:
        moveSelection(-1);
        event->accept();
        return;
    case Qt::Key_Down:
        moveSelection(+1);
        event->accept();
        return;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (auto *item = m_list ? m_list->currentItem() : nullptr) {
            onItemActivated(item);
        }
        return;
    default:
        break;
    }
    QDialog::keyPressEvent(event);
}

void CommandPalette::onQueryChanged(const QString &text)
{
    m_filter = text.trimmed();
    rebuildList();
    selectFirstSelectable();
}

void CommandPalette::onItemActivated(QListWidgetItem *item)
{
    if (!item || item->type() != kItemTypeAction) return;
    const int idx = item->data(kActionIndexRole).toInt();
    if (idx < 0 || idx >= static_cast<int>(m_actions.size())) return;

    auto trigger = m_actions[idx].trigger;  // copy, palette may close+free
    accept();
    if (trigger) trigger();
}

void CommandPalette::rebuildList()
{
    if (!m_list) return;
    m_list->clear();

    const QString needle = m_filter.toLower();
    auto matches = [&](const Action &a) {
        if (needle.isEmpty()) return true;
        return a.title.contains(needle, Qt::CaseInsensitive)
            || a.subtitle.contains(needle, Qt::CaseInsensitive)
            || a.group.contains(needle, Qt::CaseInsensitive);
    };

    // Walk the action vector once, emitting a group header the first time we
    // see each group and then the action rows. Preserves insertion order.
    QString currentGroup;
    bool currentGroupHeaderEmitted = false;

    for (size_t i = 0; i < m_actions.size(); ++i) {
        const auto &a = m_actions[i];
        if (!matches(a)) continue;

        if (a.group != currentGroup) {
            currentGroup = a.group;
            currentGroupHeaderEmitted = false;
        }
        if (!currentGroupHeaderEmitted && !currentGroup.isEmpty()) {
            auto *hdr = new QListWidgetItem(currentGroup.toUpper(), nullptr,
                                             kItemTypeGroup);
            hdr->setFlags(Qt::ItemIsEnabled);  // not selectable
            QFont f;
            f.setPointSize(9);
            f.setBold(true);
            f.setLetterSpacing(QFont::AbsoluteSpacing, 1.0);
            hdr->setFont(f);
            hdr->setForeground(QBrush(QColor(VisivoTheme::kSecondary())));
            hdr->setSizeHint(QSize(0, 26));
            hdr->setData(Qt::TextAlignmentRole,
                         int(Qt::AlignLeft | Qt::AlignVCenter));
            // Indent group headers slightly for hierarchy.
            hdr->setText(QStringLiteral("   ") + currentGroup.toUpper());
            m_list->addItem(hdr);
            currentGroupHeaderEmitted = true;
        }

        // Build the action row as a custom widget — three columns:
        // [glyph]  [title \n subtitle]                          [shortcut]
        auto *row = new QListWidgetItem(QString(), nullptr, kItemTypeAction);
        row->setData(kActionIndexRole, int(i));
        row->setSizeHint(QSize(0, 48));
        m_list->addItem(row);

        auto *rowWidget = new QWidget;
        rowWidget->setAttribute(Qt::WA_TranslucentBackground);
        auto *rl = new QHBoxLayout(rowWidget);
        rl->setContentsMargins(16, 6, 16, 6);
        rl->setSpacing(12);

        auto *glyph = new QLabel(a.glyph.isEmpty() ? u"›"_s : a.glyph, rowWidget);
        glyph->setFixedWidth(20);
        glyph->setAlignment(Qt::AlignCenter);
        glyph->setStyleSheet(
                QStringLiteral("color: %1; font-size: 14px; background: transparent;")
                        .arg(tFgDim()));
        rl->addWidget(glyph);

        auto *labelCol = new QVBoxLayout;
        labelCol->setContentsMargins(0, 0, 0, 0);
        labelCol->setSpacing(0);
        auto *title = new QLabel(a.title, rowWidget);
        title->setStyleSheet(
                QStringLiteral("color: %1; font-size: 13px; background: transparent;")
                        .arg(tFg()));
        labelCol->addWidget(title);
        if (!a.subtitle.isEmpty()) {
            auto *sub = new QLabel(a.subtitle, rowWidget);
            sub->setStyleSheet(
                    QStringLiteral("color: %1; font-size: 11px; background: transparent;")
                            .arg(tFgDim()));
            labelCol->addWidget(sub);
        }
        rl->addLayout(labelCol, 1);

        if (!a.shortcut.isEmpty()) {
            auto *kbd = new QLabel(a.shortcut, rowWidget);
            kbd->setStyleSheet(
                    QStringLiteral(
                            "background: %1; color: %2;"
                            "font-family: 'JetBrains Mono', 'SF Mono', Menlo, monospace;"
                            "font-size: 10px; padding: 2px 6px; border-radius: 3px;"
                            "border: 1px solid %3;")
                            .arg(QString::fromLatin1(VisivoTheme::kSurfaceContainerHigh()),
                                 tFgDim(), tBorder()));
            rl->addWidget(kbd);
        }

        m_list->setItemWidget(row, rowWidget);
    }

    if (m_list->count() == 0) {
        auto *empty = new QListWidgetItem(
                QStringLiteral("   No matches for \"%1\"").arg(m_filter),
                nullptr, kItemTypeGroup);
        empty->setFlags(Qt::ItemIsEnabled);
        empty->setForeground(QBrush(QColor(VisivoTheme::kSecondary())));
        empty->setSizeHint(QSize(0, 36));
        m_list->addItem(empty);
    }
}

int CommandPalette::selectableRowCount() const
{
    if (!m_list) return 0;
    int n = 0;
    for (int i = 0; i < m_list->count(); ++i) {
        if (m_list->item(i)->type() == kItemTypeAction) ++n;
    }
    return n;
}

void CommandPalette::selectFirstSelectable()
{
    if (!m_list) return;
    for (int i = 0; i < m_list->count(); ++i) {
        if (m_list->item(i)->type() == kItemTypeAction) {
            m_list->setCurrentRow(i);
            return;
        }
    }
}

void CommandPalette::moveSelection(int delta)
{
    if (!m_list || m_list->count() == 0) return;
    int row = m_list->currentRow();
    if (row < 0) row = 0;
    const int n = m_list->count();
    for (int step = 0; step < n; ++step) {
        row = (row + delta + n) % n;
        if (m_list->item(row)->type() == kItemTypeAction) {
            m_list->setCurrentRow(row);
            return;
        }
    }
}
