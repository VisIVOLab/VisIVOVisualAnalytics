// FitsHeaderEditorDialog.cpp — see FitsHeaderEditorDialog.h.

#include "FitsHeaderEditorDialog.h"

#include "FitsHeaderCard.h"

#include <QDialogButtonBox>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrent>

using namespace Qt::Literals::StringLiterals;

namespace {

enum Column { ColKey = 0, ColValue, ColComment, ColumnCount };

}   // namespace

FitsHeaderEditorDialog::FitsHeaderEditorDialog(const QString &path, const QStringList &cards,
                                               const QString &backendUrl,
                                               const QString &backendToken, QWidget *parent)
    : QDialog(parent), m_path(path), m_backendUrl(backendUrl), m_backendToken(backendToken)
{
    setWindowTitle(u"Edit FITS header — %1"_s.arg(QFileInfo(path).fileName()));
    resize(720, 520);
    buildUi();
    populate(cards);
    connect(&m_watcher, &QFutureWatcher<BackendHeaderPatchResult>::finished, this,
            &FitsHeaderEditorDialog::onFinished);
}

FitsHeaderEditorDialog::~FitsHeaderEditorDialog()
{
    m_watcher.disconnect(this);
}

void FitsHeaderEditorDialog::buildUi()
{
    auto *layout = new QVBoxLayout(this);

    auto *note = new QLabel(
            u"The file is not modified: the edits are written to a new file in the "
            u"Workspace. Structural keywords (SIMPLE, BITPIX, NAXISn) are not editable — "
            u"they describe the data on disk."_s,
            this);
    note->setWordWrap(true);
    layout->addWidget(note);

    m_table = new QTableWidget(0, ColumnCount, this);
    m_table->setHorizontalHeaderLabels({ u"Keyword"_s, u"Value"_s, u"Comment"_s });
    m_table->horizontalHeader()->setSectionResizeMode(ColValue, QHeaderView::Stretch);
    m_table->verticalHeader()->setVisible(false);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setAlternatingRowColors(true);
    layout->addWidget(m_table, 1);

    auto *rowButtons = new QHBoxLayout;
    auto *add = new QPushButton(u"Add keyword"_s, this);
    connect(add, &QPushButton::clicked, this, &FitsHeaderEditorDialog::addRow);
    rowButtons->addWidget(add);
    auto *remove = new QPushButton(u"Remove keyword"_s, this);
    connect(remove, &QPushButton::clicked, this, &FitsHeaderEditorDialog::removeSelectedRow);
    rowButtons->addWidget(remove);
    rowButtons->addStretch(1);
    rowButtons->addWidget(new QLabel(u"Save as"_s, this));
    m_outputName = new QLineEdit(this);
    m_outputName->setPlaceholderText(u"%1_hdr.fits"_s.arg(QFileInfo(m_path).completeBaseName()));
    m_outputName->setMinimumWidth(220);
    rowButtons->addWidget(m_outputName);
    layout->addLayout(rowButtons);

    m_status = new QLabel(QString(), this);
    m_status->setWordWrap(true);
    layout->addWidget(m_status);

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    connect(m_buttons, &QDialogButtonBox::accepted, this, &FitsHeaderEditorDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(m_buttons);
}

void FitsHeaderEditorDialog::populate(const QStringList &cards)
{
    for (const QString &raw : cards) {
        const visivo::FitsHeaderCard card = visivo::parseFitsHeaderCard(raw);
        if (card.key.isEmpty() || card.key == u"END"_s)
            continue;

        const int row = m_table->rowCount();
        m_table->insertRow(row);
        auto *key = new QTableWidgetItem(card.key);
        auto *value = new QTableWidgetItem(card.value);
        auto *comment = new QTableWidgetItem(card.comment);

        // The keyword of an existing card is fixed: renaming it in place would
        // read as an edit but mean "remove one, add another", which is not what
        // the row says. Remove it and add the new one instead.
        key->setFlags(key->flags() & ~Qt::ItemIsEditable);
        if (card.commentary) {
            // HISTORY / COMMENT have no single value to change.
            value->setFlags(value->flags() & ~Qt::ItemIsEditable);
            comment->setFlags(comment->flags() & ~Qt::ItemIsEditable);
        }
        // Marked on the row itself: the save pass has to skip every commentary
        // card, not just HISTORY and COMMENT. Two CONTINUE cards (or two
        // HIERARCH ones, which this parser reads under the same truncated key)
        // otherwise look like a duplicate keyword and block any edit at all.
        key->setData(Qt::UserRole, card.commentary);
        m_table->setItem(row, ColKey, key);
        m_table->setItem(row, ColValue, value);
        m_table->setItem(row, ColComment, comment);

        if (!card.commentary)
            m_original.append({ card.key, card.value, card.comment });
    }
    m_table->resizeColumnToContents(ColKey);
}

void FitsHeaderEditorDialog::addRow()
{
    const int row = m_table->rowCount();
    m_table->insertRow(row);
    m_table->setItem(row, ColKey, new QTableWidgetItem(QString()));
    m_table->setItem(row, ColValue, new QTableWidgetItem(QString()));
    m_table->setItem(row, ColComment, new QTableWidgetItem(QString()));
    m_table->setCurrentCell(row, ColKey);
    m_table->editItem(m_table->item(row, ColKey));
}

void FitsHeaderEditorDialog::removeSelectedRow()
{
    const int row = m_table->currentRow();
    if (row >= 0)
        m_table->removeRow(row);
}

void FitsHeaderEditorDialog::accept()
{
    if (m_watcher.isRunning())
        return;

    // What is on screen now, keyed by keyword.
    QHash<QString, QPair<QString, QString>> current;
    QStringList order;
    for (int row = 0; row < m_table->rowCount(); ++row) {
        const QTableWidgetItem *keyItem = m_table->item(row, ColKey);
        if (!keyItem || keyItem->data(Qt::UserRole).toBool())
            continue;   // commentary: no single value to send
        const QString key = keyItem->text().trimmed().toUpper();
        if (key.isEmpty() || key == u"HISTORY"_s || key == u"COMMENT"_s)
            continue;
        if (current.contains(key)) {
            QMessageBox::warning(this, windowTitle(),
                                 u"%1 appears twice; keywords must be unique."_s.arg(key));
            return;
        }
        current.insert(key, { m_table->item(row, ColValue)->text().trimmed(),
                              m_table->item(row, ColComment)->text().trimmed() });
        order << key;
    }

    QList<BackendHeaderCardEdit> edits;
    for (const QString &key : order) {
        const auto [value, comment] = current.value(key);
        const auto previous = std::find_if(
                m_original.cbegin(), m_original.cend(),
                [&key](const OriginalCard &c) { return c.key == key; });
        const bool isNew = previous == m_original.cend();
        if (!isNew && previous->value == value && previous->comment == comment)
            continue;
        // A value that reads as a number but replaces one that was a string
        // goes out quoted: otherwise an OBJECT of "12345" stops being a name
        // and becomes an integer card.
        const bool wasString = !isNew && !previous->value.isEmpty()
                && !visivo::needsQuoting(previous->value);
        const QString outgoing = (wasString && visivo::needsQuoting(value))
                ? u"'%1'"_s.arg(value)
                : value;
        // The dialog shows the comment column, so it always has one to send —
        // including an empty one, which is how a comment is cleared here.
        edits.append({ key, outgoing, comment, /*commentProvided=*/true });
    }

    QStringList removed;
    for (const OriginalCard &card : std::as_const(m_original)) {
        if (!current.contains(card.key))
            removed << card.key;
    }

    if (edits.isEmpty() && removed.isEmpty()) {
        QMessageBox::information(this, windowTitle(), u"Nothing was changed."_s);
        return;
    }

    m_buttons->setEnabled(false);
    m_status->setText(u"Writing the corrected copy…"_s);

    const QString url = m_backendUrl;
    const QString token = m_backendToken;
    const QString path = m_path;
    const QString name = m_outputName->text().trimmed();
    m_watcher.setFuture(QtConcurrent::run([url, token, path, edits, removed, name]() {
        BackendClient client(url, token);
        return client.patchFitsHeader(path, edits, removed, name);
    }));
}

void FitsHeaderEditorDialog::onFinished()
{
    const BackendHeaderPatchResult result = m_watcher.result();
    if (!result.valid) {
        m_buttons->setEnabled(true);
        m_status->setText(QString());
        QMessageBox box(QMessageBox::Critical, windowTitle(),
                        result.error.isEmpty() ? u"The header could not be written."_s
                                               : result.error,
                        QMessageBox::Ok, this);
        box.setTextFormat(Qt::PlainText);
        box.exec();
        return;
    }

    // The legacy said "File has been saved!" and stopped. What matters is
    // whether the edit achieved anything, which is what the backend reports.
    QMessageBox box(this);
    box.setWindowTitle(windowTitle());
    box.setTextFormat(Qt::PlainText);
    box.setIcon(result.wcsCelestial ? QMessageBox::Information : QMessageBox::Warning);
    box.setText(u"%1\n\n%2"_s.arg(result.workspaceFilename, result.wcsMessage));
    box.setDetailedText(result.changes.join(u"\n"_s));
    auto *open = box.addButton(u"Open It"_s, QMessageBox::AcceptRole);
    box.addButton(u"Close"_s, QMessageBox::RejectRole);
    box.setDefaultButton(open);
    box.exec();

    if (box.clickedButton() == open)
        emit saved(result.outputPath);
    QDialog::accept();
}
