#include "BackendsSettingsWidget.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QTableWidget>
#include <QVBoxLayout>

using namespace Qt::StringLiterals;

namespace {

QString sanitizeId(const QString &raw)
{
    QString id = raw.trimmed().toLower();
    static const QRegularExpression notAllowed(u"[^a-z0-9._-]"_s);
    id.replace(notAllowed, u"-"_s);
    return id;
}

/// In-place editor for a single BackendNode. Returns true on accept.
bool editNodeDialog(QWidget *parent, BackendNode &node, bool allowIdEdit,
                    const QStringList &usedIds)
{
    QDialog dlg(parent);
    dlg.setWindowTitle(allowIdEdit ? u"Add Backend Node"_s : u"Edit Backend Node"_s);
    dlg.setMinimumWidth(480);

    auto *form = new QFormLayout;

    auto *idEdit = new QLineEdit(node.id, &dlg);
    idEdit->setPlaceholderText(u"e.g. src-it-inaf"_s);
    idEdit->setEnabled(allowIdEdit);
    form->addRow(u"Identifier:"_s, idEdit);

    auto *nameEdit = new QLineEdit(node.name, &dlg);
    nameEdit->setPlaceholderText(u"e.g. INAF SRC (Catania)"_s);
    form->addRow(u"Display name:"_s, nameEdit);

    auto *urlEdit = new QLineEdit(node.url, &dlg);
    urlEdit->setPlaceholderText(u"http://host.example.org:8001"_s);
    form->addRow(u"URL:"_s, urlEdit);

    auto *tokenEdit = new QLineEdit(node.token, &dlg);
    tokenEdit->setEchoMode(QLineEdit::Password);
    tokenEdit->setPlaceholderText(u"Bearer token (leave empty for unauthenticated)"_s);
    form->addRow(u"Token:"_s, tokenEdit);

    auto *srcEdit = new QLineEdit(node.srcCode, &dlg);
    srcEdit->setPlaceholderText(u"SKAVA Node.code (optional, matches DataLink descriptor)"_s);
    form->addRow(u"SRC code:"_s, srcEdit);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    auto *layout = new QVBoxLayout(&dlg);
    layout->addLayout(form);
    layout->addWidget(buttons);

    while (true) {
        if (dlg.exec() != QDialog::Accepted) {
            return false;
        }
        const QString id = sanitizeId(idEdit->text());
        const QString name = nameEdit->text().trimmed();
        const QString url = urlEdit->text().trimmed();
        if (id.isEmpty() || id == u"local"_s) {
            QMessageBox::warning(&dlg, u"Invalid identifier"_s,
                                 u"Identifier must be non-empty and different from \"local\"."_s);
            continue;
        }
        if (allowIdEdit && usedIds.contains(id)) {
            QMessageBox::warning(&dlg, u"Duplicate identifier"_s,
                                 u"A backend node with this identifier already exists."_s);
            continue;
        }
        if (name.isEmpty()) {
            QMessageBox::warning(&dlg, u"Missing name"_s,
                                 u"Please give the backend a display name."_s);
            continue;
        }
        if (url.isEmpty() || !(url.startsWith(u"http://"_s) || url.startsWith(u"https://"_s))) {
            QMessageBox::warning(&dlg, u"Invalid URL"_s,
                                 u"URL must start with http:// or https://"_s);
            continue;
        }
        node.id = id;
        node.name = name;
        node.url = url;
        node.token = tokenEdit->text();
        node.srcCode = srcEdit->text().trimmed();
        node.isLocal = false;
        node.isBuiltin = false;
        return true;
    }
}

} // namespace

BackendsSettingsWidget::BackendsSettingsWidget(Settings *settings, QWidget *parent)
    : QWidget(parent), m_settings(settings)
{
    auto *box = new QGroupBox(u"Remote Backends"_s, this);

    m_table = new QTableWidget(0, 4, this);
    m_table->setHorizontalHeaderLabels({ u"ID"_s, u"Name"_s, u"URL"_s, u"SRC code"_s });
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_table->verticalHeader()->setVisible(false);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);

    m_addBtn = new QPushButton(u"Add…"_s, this);
    m_editBtn = new QPushButton(u"Edit…"_s, this);
    m_removeBtn = new QPushButton(u"Remove"_s, this);
    m_editBtn->setEnabled(false);
    m_removeBtn->setEnabled(false);

    auto *helpLabel = new QLabel(
            u"Backends registered here are matched against SKAVA's "
            u"<i>visivo-backend</i> service descriptor (by URL or SRC code). "
            u"When a dataset is served by one of them, VisIVO sends compute "
            u"requests there instead of downloading the file locally. "
            u"The built-in <b>local</b> backend is configured above and is "
            u"always the fallback."_s,
            this);
    helpLabel->setWordWrap(true);

    auto *buttonsLayout = new QHBoxLayout;
    buttonsLayout->addWidget(m_addBtn);
    buttonsLayout->addWidget(m_editBtn);
    buttonsLayout->addWidget(m_removeBtn);
    buttonsLayout->addStretch();

    auto *boxLayout = new QVBoxLayout(box);
    boxLayout->addWidget(helpLabel);
    boxLayout->addWidget(m_table);
    boxLayout->addLayout(buttonsLayout);

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(box);

    QObject::connect(m_addBtn, &QPushButton::clicked, this, &BackendsSettingsWidget::addNode);
    QObject::connect(m_editBtn, &QPushButton::clicked, this, &BackendsSettingsWidget::editSelected);
    QObject::connect(m_removeBtn, &QPushButton::clicked, this,
                     &BackendsSettingsWidget::removeSelected);
    QObject::connect(m_table, &QTableWidget::itemSelectionChanged, this,
                     &BackendsSettingsWidget::onSelectionChanged);
    QObject::connect(m_table, &QTableWidget::itemDoubleClicked, this,
                     [this](QTableWidgetItem *) { editSelected(); });

    if (m_settings) {
        for (const BackendNode &n : m_settings->getBackendNodes()) {
            if (!n.isLocal) {
                m_nodes.append(n);
            }
        }
    }
    rebuildTable();
}

void BackendsSettingsWidget::saveTo(Settings *settings)
{
    if (!settings) {
        return;
    }
    // Preserve the local node — only the remote list is editable here.
    QList<BackendNode> merged;
    for (const BackendNode &n : settings->getBackendNodes()) {
        if (n.isLocal) {
            merged.append(n);
            break;
        }
    }
    merged.append(m_nodes);
    settings->setBackendNodes(merged);
}

void BackendsSettingsWidget::addNode()
{
    QStringList used;
    for (const BackendNode &n : m_nodes) {
        used.append(n.id);
    }
    BackendNode node;
    if (editNodeDialog(this, node, /*allowIdEdit=*/true, used)) {
        m_nodes.append(node);
        appendRow(node);
        m_table->setCurrentCell(m_nodes.size() - 1, 0);
    }
}

void BackendsSettingsWidget::editSelected()
{
    const int row = m_table->currentRow();
    if (row < 0 || row >= m_nodes.size()) {
        return;
    }
    BackendNode node = m_nodes.at(row);
    if (editNodeDialog(this, node, /*allowIdEdit=*/false, {})) {
        m_nodes[row] = node;
        rebuildTable();
        m_table->setCurrentCell(row, 0);
    }
}

void BackendsSettingsWidget::removeSelected()
{
    const int row = m_table->currentRow();
    if (row < 0 || row >= m_nodes.size()) {
        return;
    }
    const QString name = m_nodes.at(row).name;
    if (QMessageBox::question(this, u"Remove backend"_s,
                              u"Remove backend \"%1\"?"_s.arg(name))
        != QMessageBox::Yes) {
        return;
    }
    m_nodes.removeAt(row);
    rebuildTable();
}

void BackendsSettingsWidget::onSelectionChanged()
{
    const bool hasSelection = m_table->currentRow() >= 0;
    m_editBtn->setEnabled(hasSelection);
    m_removeBtn->setEnabled(hasSelection);
}

void BackendsSettingsWidget::rebuildTable()
{
    m_table->setRowCount(0);
    for (const BackendNode &n : m_nodes) {
        appendRow(n);
    }
    onSelectionChanged();
}

void BackendsSettingsWidget::appendRow(const BackendNode &node)
{
    const int row = m_table->rowCount();
    m_table->insertRow(row);
    m_table->setItem(row, 0, new QTableWidgetItem(node.id));
    m_table->setItem(row, 1, new QTableWidgetItem(node.name));
    m_table->setItem(row, 2, new QTableWidgetItem(node.url));
    m_table->setItem(row, 3, new QTableWidgetItem(node.srcCode));
}
