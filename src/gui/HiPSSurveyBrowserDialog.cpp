#include "HiPSSurveyBrowserDialog.h"

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QtConcurrent>

using namespace Qt::StringLiterals;

namespace {

constexpr int kUrlRole   = Qt::UserRole + 1;
constexpr int kTitleRole = Qt::UserRole + 2;
constexpr int kTextRole  = Qt::UserRole + 3;   // everything the filter matches on

// Publishers write categories as a path, "Image/Infrared/HERSCHEL". The first
// component is enough to group by and keeps the tree readable.
QString topCategory(const QString &category)
{
    const QString trimmed = category.trimmed();
    if (trimmed.isEmpty())
        return u"Uncategorised"_s;
    const int slash = trimmed.indexOf(u'/');
    return slash > 0 ? trimmed.left(slash) : trimmed;
}

} // namespace

HiPSSurveyBrowserDialog::HiPSSurveyBrowserDialog(const QString &backendUrl,
                                                 const QString &backendToken,
                                                 QWidget *parent)
    : QDialog(parent)
    , m_backendUrl(backendUrl)
    , m_backendToken(backendToken)
{
    setWindowTitle(u"Choose a HiPS Survey"_s);
    resize(720, 520);

    auto *root = new QVBoxLayout(this);

    auto *filterRow = new QHBoxLayout;
    filterRow->addWidget(new QLabel(u"Search:"_s, this));
    m_filterEdit = new QLineEdit(this);
    m_filterEdit->setPlaceholderText(u"name, wavelength or category — e.g. 2MASS, radio, Herschel"_s);
    m_filterEdit->setClearButtonEnabled(true);
    QObject::connect(m_filterEdit, &QLineEdit::textChanged,
                     this, &HiPSSurveyBrowserDialog::onFilterChanged);
    filterRow->addWidget(m_filterEdit, 1);

    m_refreshButton = new QPushButton(u"Refresh"_s, this);
    m_refreshButton->setToolTip(u"Fetch the list from CDS again instead of using the cached copy."_s);
    QObject::connect(m_refreshButton, &QPushButton::clicked,
                     this, &HiPSSurveyBrowserDialog::onRefresh);
    filterRow->addWidget(m_refreshButton);
    root->addLayout(filterRow);

    m_tree = new QTreeWidget(this);
    m_tree->setColumnCount(3);
    m_tree->setHeaderLabels({ u"Survey"_s, u"Regime"_s, u"Max order"_s });
    m_tree->setRootIsDecorated(true);
    m_tree->setUniformRowHeights(true);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    QObject::connect(m_tree, &QTreeWidget::itemSelectionChanged,
                     this, &HiPSSurveyBrowserDialog::onSelectionChanged);
    QObject::connect(m_tree, &QTreeWidget::itemActivated,
                     this, &HiPSSurveyBrowserDialog::onItemActivated);
    root->addWidget(m_tree, 1);

    m_statusLabel = new QLabel(u"Loading the survey list…"_s, this);
    root->addWidget(m_statusLabel);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    m_openButton = buttons->addButton(u"Open"_s, QDialogButtonBox::AcceptRole);
    m_openButton->setEnabled(false);
    QObject::connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(buttons);

    QObject::connect(&m_watcher, &QFutureWatcher<BackendHiPSRegistryResult>::finished,
                     this, &HiPSSurveyBrowserDialog::onLoaded);
    startLoad(false);
}

void HiPSSurveyBrowserDialog::startLoad(bool refresh)
{
    if (m_watcher.isRunning()) return;
    if (m_refreshButton) m_refreshButton->setEnabled(false);
    if (m_statusLabel)
        m_statusLabel->setText(refresh ? u"Fetching the survey list from CDS…"_s
                                       : u"Loading the survey list…"_s);

    const QString url = m_backendUrl;
    const QString token = m_backendToken;
    m_watcher.setFuture(QtConcurrent::run([url, token, refresh]() {
        BackendClient client(url, token);
        return client.requestHiPSSurveys(refresh);
    }));
}

void HiPSSurveyBrowserDialog::onRefresh()
{
    startLoad(true);
}

void HiPSSurveyBrowserDialog::onLoaded()
{
    const BackendHiPSRegistryResult result = m_watcher.result();
    if (m_refreshButton) m_refreshButton->setEnabled(true);

    if (!result.valid) {
        if (m_statusLabel)
            m_statusLabel->setText(result.error.isEmpty()
                                           ? u"The survey list could not be read."_s
                                           : result.error);
        return;
    }
    m_surveys = result.surveys;
    populate();
}

void HiPSSurveyBrowserDialog::populate()
{
    if (!m_tree) return;
    m_tree->clear();

    QHash<QString, QTreeWidgetItem *> groups;
    for (const auto &survey : std::as_const(m_surveys)) {
        if (survey.url.isEmpty()) continue;
        const QString group = topCategory(survey.category);
        QTreeWidgetItem *parent = groups.value(group);
        if (!parent) {
            parent = new QTreeWidgetItem(m_tree, { group });
            QFont bold = parent->font(0);
            bold.setBold(true);
            parent->setFont(0, bold);
            parent->setFlags(parent->flags() & ~Qt::ItemIsSelectable);
            groups.insert(group, parent);
        }

        auto *item = new QTreeWidgetItem(parent);
        item->setText(0, survey.title.isEmpty() ? survey.id : survey.title);
        item->setText(1, survey.regime);
        item->setText(2, survey.maxOrder > 0 ? QString::number(survey.maxOrder) : QString());
        item->setToolTip(0, QStringLiteral("%1\n%2").arg(survey.id, survey.url));
        item->setData(0, kUrlRole, survey.url);
        item->setData(0, kTitleRole, survey.title);
        item->setData(0, kTextRole,
                      QStringLiteral("%1 %2 %3 %4")
                              .arg(survey.title, survey.id, survey.category, survey.regime)
                              .toLower());
    }

    m_tree->sortItems(0, Qt::AscendingOrder);
    onFilterChanged(m_filterEdit ? m_filterEdit->text() : QString());
}

void HiPSSurveyBrowserDialog::onFilterChanged(const QString &text)
{
    if (!m_tree) return;

    // Every word has to appear somewhere in the row, so "herschel 250" narrows
    // rather than widening as it would with a single substring match.
    const QStringList terms = text.toLower().split(u' ', Qt::SkipEmptyParts);

    int shown = 0;
    for (int g = 0; g < m_tree->topLevelItemCount(); ++g) {
        QTreeWidgetItem *group = m_tree->topLevelItem(g);
        int visibleChildren = 0;
        for (int c = 0; c < group->childCount(); ++c) {
            QTreeWidgetItem *child = group->child(c);
            const QString haystack = child->data(0, kTextRole).toString();
            bool match = true;
            for (const QString &term : terms) {
                if (!haystack.contains(term)) { match = false; break; }
            }
            child->setHidden(!match);
            // Hiding a row does not deselect it, so Open would still have
            // accepted a survey the filter had just excluded.
            if (!match && child->isSelected())
                child->setSelected(false);
            if (match) ++visibleChildren;
        }
        group->setHidden(visibleChildren == 0);
        if (visibleChildren > 0 && !terms.isEmpty())
            group->setExpanded(true);
        shown += visibleChildren;
    }
    onSelectionChanged();

    if (m_statusLabel) {
        m_statusLabel->setText(
                terms.isEmpty()
                        ? QStringLiteral("%1 image surveys.").arg(m_surveys.size())
                        : QStringLiteral("%1 of %2 surveys match.")
                                  .arg(shown).arg(m_surveys.size()));
    }
}

void HiPSSurveyBrowserDialog::onSelectionChanged()
{
    m_selectedUrl.clear();
    m_selectedTitle.clear();
    if (m_tree) {
        const auto selected = m_tree->selectedItems();
        if (!selected.isEmpty()) {
            m_selectedUrl   = selected.first()->data(0, kUrlRole).toString();
            m_selectedTitle = selected.first()->data(0, kTitleRole).toString();
        }
    }
    if (m_openButton) m_openButton->setEnabled(!m_selectedUrl.isEmpty());
}

void HiPSSurveyBrowserDialog::onItemActivated(QTreeWidgetItem *item, int column)
{
    Q_UNUSED(column);
    if (!item || item->data(0, kUrlRole).toString().isEmpty()) return;
    m_selectedUrl   = item->data(0, kUrlRole).toString();
    m_selectedTitle = item->data(0, kTitleRole).toString();
    accept();
}
