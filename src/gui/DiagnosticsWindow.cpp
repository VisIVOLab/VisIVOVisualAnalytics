#include "DiagnosticsWindow.h"

#include "app/DiagnosticsManager.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QPushButton>
#include <QTableView>
#include <QVBoxLayout>

DiagnosticsFilterModel::DiagnosticsFilterModel(QObject *parent) : QSortFilterProxyModel(parent)
{
}

void DiagnosticsFilterModel::setLevelFilter(const QString &level)
{
    this->m_levelFilter = level;
    this->invalidateFilter();
}

void DiagnosticsFilterModel::setCategoryFilter(const QString &category)
{
    this->m_categoryFilter = category;
    this->invalidateFilter();
}

bool DiagnosticsFilterModel::filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const
{
    const auto *model = qobject_cast<const DiagnosticsModel *>(this->sourceModel());
    if (!model) {
        return true;
    }
    const DiagnosticEntry entry = model->entryAt(sourceRow);
    if (!this->m_levelFilter.isEmpty() && this->m_levelFilter != QStringLiteral("All")
        && DiagnosticsManager::levelToString(entry.level) != this->m_levelFilter) {
        return false;
    }
    if (!this->m_categoryFilter.isEmpty() && this->m_categoryFilter != QStringLiteral("All")
        && DiagnosticsManager::categoryToString(entry.category) != this->m_categoryFilter) {
        return false;
    }
    return QSortFilterProxyModel::filterAcceptsRow(sourceRow, sourceParent);
}

DiagnosticsWindow::DiagnosticsWindow(QWidget *parent, Qt::WindowFlags flags)
    : QWidget(parent, flags)
{
    this->setWindowTitle(QStringLiteral("Diagnostics"));
    this->resize(1100, 480);
    this->setAttribute(Qt::WA_DeleteOnClose, false);

    this->m_model = new DiagnosticsModel(DiagnosticsManager::instance(), this);
    this->m_filterModel = new DiagnosticsFilterModel(this);
    this->m_filterModel->setSourceModel(this->m_model);

    this->m_tableView = new QTableView(this);
    this->m_tableView->setModel(this->m_filterModel);
    this->m_tableView->setSelectionBehavior(QAbstractItemView::SelectRows);
    this->m_tableView->setSelectionMode(QAbstractItemView::ExtendedSelection);
    this->m_tableView->setAlternatingRowColors(true);
    this->m_tableView->setSortingEnabled(false);
    this->m_tableView->verticalHeader()->setVisible(false);
    this->m_tableView->horizontalHeader()->setStretchLastSection(true);
    this->m_tableView->horizontalHeader()->setSectionResizeMode(DiagnosticsModel::TimeColumn,
                                                                QHeaderView::ResizeToContents);
    this->m_tableView->horizontalHeader()->setSectionResizeMode(DiagnosticsModel::LevelColumn,
                                                                QHeaderView::ResizeToContents);
    this->m_tableView->horizontalHeader()->setSectionResizeMode(DiagnosticsModel::CategoryColumn,
                                                                QHeaderView::ResizeToContents);
    this->m_tableView->horizontalHeader()->setSectionResizeMode(DiagnosticsModel::SourceColumn,
                                                                QHeaderView::ResizeToContents);
    this->m_tableView->horizontalHeader()->setSectionResizeMode(DiagnosticsModel::OperationColumn,
                                                                QHeaderView::ResizeToContents);
    this->m_tableView->horizontalHeader()->setSectionResizeMode(DiagnosticsModel::ContextColumn,
                                                                QHeaderView::ResizeToContents);

    this->m_levelCombo = new QComboBox(this);
    this->m_levelCombo->addItems({ QStringLiteral("All"), QStringLiteral("Debug"),
                                   QStringLiteral("Info"), QStringLiteral("Warning"),
                                   QStringLiteral("Error") });
    this->m_categoryCombo = new QComboBox(this);
    this->m_categoryCombo->addItems(
            { QStringLiteral("All"), QStringLiteral("Scientific"), QStringLiteral("Client"),
              QStringLiteral("Backend"), QStringLiteral("Task"), QStringLiteral("WCS"),
              QStringLiteral("Remote"), QStringLiteral("Rendering"),
              QStringLiteral("Performance") });
    this->m_autoScrollCheck = new QCheckBox(QStringLiteral("Auto-scroll"), this);
    this->m_autoScrollCheck->setChecked(true);
    this->m_copyButton = new QPushButton(QStringLiteral("Copy Selected"), this);
    this->m_clearButton = new QPushButton(QStringLiteral("Clear"), this);

    auto *controlsLayout = new QHBoxLayout();
    controlsLayout->addWidget(this->m_levelCombo);
    controlsLayout->addWidget(this->m_categoryCombo);
    controlsLayout->addStretch(1);
    controlsLayout->addWidget(this->m_autoScrollCheck);
    controlsLayout->addWidget(this->m_copyButton);
    controlsLayout->addWidget(this->m_clearButton);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(controlsLayout);
    layout->addWidget(this->m_tableView);
    layout->setContentsMargins(8, 8, 8, 8);

    QObject::connect(this->m_levelCombo, &QComboBox::currentTextChanged, this->m_filterModel,
                     &DiagnosticsFilterModel::setLevelFilter);
    QObject::connect(this->m_categoryCombo, &QComboBox::currentTextChanged, this->m_filterModel,
                     &DiagnosticsFilterModel::setCategoryFilter);
    QObject::connect(this->m_copyButton, &QPushButton::clicked, this,
                     &DiagnosticsWindow::copySelectedMessages);
    QObject::connect(this->m_clearButton, &QPushButton::clicked, DiagnosticsManager::instance(),
                     &DiagnosticsManager::clear);
    QObject::connect(DiagnosticsManager::instance(), &DiagnosticsManager::entryAppended, this, [this]() {
        if (this->m_autoScrollCheck->isChecked()) {
            this->m_tableView->scrollToBottom();
        }
    });
}

void DiagnosticsWindow::copySelectedMessages()
{
    const QModelIndexList rows = this->m_tableView->selectionModel()->selectedRows();
    QStringList lines;
    for (const QModelIndex &proxyIndex : rows) {
        const QModelIndex index = this->m_filterModel->mapToSource(proxyIndex);
        const DiagnosticEntry entry = this->m_model->entryAt(index.row());
        lines << QStringLiteral("%1\t%2\t%3\t%4\t%5\t%6")
                         .arg(entry.timestamp.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")),
                              DiagnosticsManager::levelToString(entry.level),
                              DiagnosticsManager::categoryToString(entry.category), entry.source,
                              entry.operationTag, entry.message);
    }
    if (!lines.isEmpty()) {
        QGuiApplication::clipboard()->setText(lines.join(u'\n'));
    }
}
