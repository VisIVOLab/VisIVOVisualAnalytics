#include "SourceFindDialog.h"

#include "theme/ToolDialogStyle.h"
#include "theme/VisivoTheme.h"

#include <QApplication>
#include <QColor>
#include <QComboBox>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QInputDialog>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTime>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace {

enum ParamColumn { ColKey, ColValue, ColStatus, ColDefault, ColReason, ColCount };

/// The words for an option of the suggester's two questions.
QString optionLabel(const QString &option)
{
    if (option == u"emission"_s)
        return u"Emission"_s;
    if (option == u"absorption"_s)
        return u"Absorption (searches the inverted cube)"_s;
    if (option == u"continuum"_s)
        return u"Continuum not subtracted"_s;
    if (option == u"hi"_s)
        return u"HI (lines of 15-120 km/s)"_s;
    if (option == u"galactic"_s)
        return u"Galactic lines (2-20 km/s)"_s;
    return option;
}

/// How a deduction came to be, for Provenance.
QString choiceRecord(const visivo::SofiaChoice &c)
{
    if (!c.present)
        return {};
    if (c.confidence == u"user"_s || !c.deduced)
        return u"%1 (your choice)"_s.arg(c.value);
    return c.confidence.isEmpty() ? u"%1 (deduced)"_s.arg(c.value)
                                  : u"%1 (deduced, %2 confidence)"_s.arg(c.value, c.confidence);
}

QString elapsedText(qint64 ms)
{
    const qint64 s = ms / 1000;
    return u"%1:%2"_s.arg(s / 60).arg(s % 60, 2, 10, QLatin1Char('0'));
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────

SourceFindDialog::SourceFindDialog(std::shared_ptr<BackendClient> client,
                                   const QString &datasetId,
                                   QWidget *parent)
    : QDialog(parent)
    , m_client(std::move(client))
    , m_datasetId(datasetId)
    , m_cancel(std::make_shared<std::atomic_bool>(false))
{
    setWindowTitle(u"SoFiA-2 Source Finding"_s);
    setMinimumWidth(760);
    resize(960, 820);
    // Shared input theme — see ToolDialogStyle.
    setStyleSheet(ToolDialogStyle::inputCss());

    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(12, 12, 12, 12);
    mainLayout->setSpacing(8);
    mainLayout->addWidget(ToolDialogStyle::makeBreadcrumb(this, u"Source Finding"_s));

    // ── Suggestion ────────────────────────────────────────────────────────────
    // Asked for on opening, not behind a button: it reads a sample of the cube
    // (about a second, memory-mapped), and a dialog that opened on SoFiA's
    // generic defaults would make those the path of least resistance, which is
    // what the suggestion exists to replace.
    QFormLayout *form = ToolDialogStyle::addSection(mainLayout, u"Suggested from the data"_s, this);

    const auto choiceRow = [this, form](const QString &label, QComboBox *&combo, QLabel *&note,
                                        QPushButton *&confirm) {
        combo = new QComboBox(this);
        confirm = VisivoTheme::makeSecondaryButton(u"Confirm"_s, this);
        confirm->setVisible(false);
        auto *row = new QHBoxLayout();
        row->addWidget(combo, 1);
        row->addWidget(confirm);
        form->addRow(label, row);
        note = new QLabel(this);
        note->setWordWrap(true);
        note->setStyleSheet(ToolDialogStyle::helperCss());
        form->addRow(QString(), note);
    };
    choiceRow(u"Data type"_s, m_dataTypeCombo, m_dataTypeNote, m_dataTypeConfirm);
    choiceRow(u"Line widths"_s, m_lineCombo, m_lineNote, m_lineConfirm);

    m_statusLabel = new QLabel(this);
    m_statusLabel->setWordWrap(true);
    m_statusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    mainLayout->addWidget(m_statusLabel);
    m_warningsLabel = new QLabel(this);
    m_warningsLabel->setWordWrap(true);
    m_warningsLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_warningsLabel->setStyleSheet(u"color: %1;"_s.arg(QString::fromLatin1(VisivoTheme::kWarning())));
    m_warningsLabel->setVisible(false);
    mainLayout->addWidget(m_warningsLabel);

    m_paramTable = new QTableWidget(0, ColCount, this);
    m_paramTable->setHorizontalHeaderLabels(
            { u"Parameter"_s, u"Value"_s, u"Status"_s, u"SoFiA default"_s, u"Why"_s });
    m_paramTable->verticalHeader()->setVisible(false);
    // A row is never shorter than the line editor a double-click opens in it,
    // or the value being edited is cut.
    m_paramTable->verticalHeader()->setMinimumSectionSize(QLineEdit().sizeHint().height() + 2);
    m_paramTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_paramTable->setWordWrap(true);
    m_paramTable->horizontalHeader()->setStretchLastSection(true);
    // The reason wraps to its column: when the dialog's width changes it, the
    // rows take their new height.
    connect(m_paramTable->horizontalHeader(), &QHeaderView::sectionResized, this,
            [this](int column) {
        if (column == ColReason && !m_fillingTable)
            m_paramTable->resizeRowsToContents();
    });
    m_paramTable->horizontalHeaderItem(ColValue)->setToolTip(
            u"Double-click to edit. Your value is kept when the suggestion is redone; "
            u"a blank value leaves SoFiA's default."_s);
    // Parameters above, result below, in a splitter: before a run the table
    // takes the height (the result is one empty line), after it the sources
    // get their share and the user can move the handle.
    m_splitter = new QSplitter(Qt::Vertical, this);
    m_splitter->setChildrenCollapsible(false);
    auto *paramPanel = new QWidget(m_splitter);
    auto *paramLayout = new QVBoxLayout(paramPanel);
    paramLayout->setContentsMargins(0, 0, 0, 0);
    paramLayout->addWidget(m_paramTable, 1);

    auto *paramButtons = new QHBoxLayout();
    m_resetBtn = VisivoTheme::makeSecondaryButton(u"Reset to suggested"_s, this);
    m_resetBtn->setToolTip(u"Put every value you edited back to the suggestion."_s);
    m_suggestBtn = VisivoTheme::makeSecondaryButton(u"Suggest again"_s, this);
    m_suggestBtn->setToolTip(u"Ask the backend again (after a busy or failed answer)."_s);
    m_addBtn = VisivoTheme::makeSecondaryButton(u"Add parameter…"_s, this);
    m_addBtn->setToolTip(u"Set a SoFiA-2 parameter the suggestion leaves out, e.g. "
                         u"dilation.enable for masks that take in the faint outskirts."_s);
    paramButtons->addWidget(m_resetBtn);
    paramButtons->addWidget(m_addBtn);
    paramButtons->addWidget(m_suggestBtn);
    paramButtons->addStretch(1);
    paramLayout->addLayout(paramButtons);

    // ── Run and result ────────────────────────────────────────────────────────
    auto *resultPanel = new QWidget(m_splitter);
    auto *resultLayout = new QVBoxLayout(resultPanel);
    resultLayout->setContentsMargins(0, 6, 0, 0);
    auto *resultHeader = new QLabel(u"Detected sources"_s, resultPanel);
    resultHeader->setStyleSheet(ToolDialogStyle::sectionCss());
    resultLayout->addWidget(resultHeader);
    auto *runRow = new QHBoxLayout();
    m_progressBar = new QProgressBar(this);
    // Indeterminate: the task's progress is a timer on the backend, not
    // SoFiA's; a made-up percentage is worse than an honest "still running".
    m_progressBar->setRange(0, 0);
    m_progressBar->setVisible(false);
    m_runLabel = new QLabel(this);
    m_runLabel->setStyleSheet(ToolDialogStyle::helperCss());
    runRow->addWidget(m_progressBar, 1);
    runRow->addWidget(m_runLabel, 2);
    resultLayout->addLayout(runRow);
    m_summaryLabel = new QPlainTextEdit(this);
    m_summaryLabel->setReadOnly(true);
    m_summaryLabel->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    m_summaryLabel->setFrameShape(QFrame::NoFrame);
    m_summaryLabel->setPlaceholderText(u"The outcome of a run appears here."_s);
    // A few lines on screen; SoFiA's error text scrolls rather than being cut.
    m_summaryLabel->setFixedHeight(m_summaryLabel->fontMetrics().lineSpacing() * 5 + 12);
    resultLayout->addWidget(m_summaryLabel);

    m_table = new QTableWidget(0, 0, this);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_table->setVisible(false);   // until there are sources to list
    resultLayout->addWidget(m_table, 1);
    resultLayout->addStretch(0);

    m_splitter->addWidget(paramPanel);
    m_splitter->addWidget(resultPanel);
    m_splitter->setStretchFactor(0, 1);
    m_splitter->setStretchFactor(1, 0);
    mainLayout->addWidget(m_splitter, 1);
    // Proportions, scaled by QSplitter to the height it gets.
    m_splitter->setSizes({ 640, 190 });

    m_elapsedTimer = new QTimer(this);
    m_elapsedTimer->setInterval(1000);
    connect(m_elapsedTimer, &QTimer::timeout, this, [this]() {
        m_runLabel->setText(u"SoFiA-2 is running on the backend… %1"_s.arg(
                elapsedText(m_runClock.elapsed())));
    });

    // ── Footer: Close (secondary) + Run (primary), right-aligned.
    auto *btnRow = new QHBoxLayout();
    btnRow->addStretch();
    auto *closeBtn = VisivoTheme::makeSecondaryButton(u"Close"_s, this);
    m_openMaskBtn  = VisivoTheme::makeSecondaryButton(u"Open mask in 3D"_s, this);
    m_openMaskBtn->setEnabled(false);
    m_openMaskBtn->setToolTip(
            u"Open the SoFiA-2 binary mask cube in the 3D viewer to inspect "
            u"the detections in 3D. Run SoFiA-2 first."_s);
    m_cancelBtn    = VisivoTheme::makeSecondaryButton(u"Cancel run"_s, this);
    m_cancelBtn->setEnabled(false);
    m_runBtn       = VisivoTheme::makePrimaryButton(u"Run SoFiA-2"_s, this);
    btnRow->addWidget(closeBtn);
    btnRow->addWidget(m_openMaskBtn);
    btnRow->addWidget(m_cancelBtn);
    btnRow->addWidget(m_runBtn);
    mainLayout->addLayout(btnRow);

    // `activated`, not `currentIndexChanged`: only the user's pick asks again,
    // never the dialog filling the combo from an answer.
    // Picking a value and confirming the pre-selected one are both answers.
    const auto answerDataType = [this]() {
        m_ask.pending.dataType = m_dataTypeCombo->currentData().toString();
        requestSuggestion();
    };
    const auto answerLine = [this]() {
        m_ask.pending.line = m_lineCombo->currentData().toString();
        requestSuggestion();
    };
    connect(m_dataTypeCombo, &QComboBox::activated, this, answerDataType);
    connect(m_lineCombo, &QComboBox::activated, this, answerLine);
    connect(m_dataTypeConfirm, &QPushButton::clicked, this, answerDataType);
    connect(m_lineConfirm, &QPushButton::clicked, this, answerLine);
    connect(m_paramTable, &QTableWidget::itemChanged, this, &SourceFindDialog::onCellChanged);
    connect(m_resetBtn, &QPushButton::clicked, this, [this]() {
        m_params.resetToSuggested();
        fillTable();
        updateControls();
    });
    connect(m_suggestBtn, &QPushButton::clicked, this, &SourceFindDialog::requestSuggestion);
    connect(m_addBtn, &QPushButton::clicked, this, &SourceFindDialog::onAddParameter);
    connect(m_runBtn,  &QPushButton::clicked, this, &SourceFindDialog::onRun);
    connect(m_cancelBtn, &QPushButton::clicked, this, &SourceFindDialog::onCancel);
    connect(closeBtn,  &QPushButton::clicked, this, &QDialog::close);
    connect(m_openMaskBtn, &QPushButton::clicked, this, [this]() {
        if (!m_maskPath.isEmpty())
            emit openMaskCubeRequested(m_maskPath);
    });

    requestSuggestion();
}

SourceFindDialog::~SourceFindDialog()
{
    // However the dialog went away, the run must not go on without it.
    m_cancel->store(true);
}

void SourceFindDialog::reject()
{
    // Escape: through closeEvent, which asks before abandoning a run.
    close();
}

// ── Suggestion ──────────────────────────────────────────────────────────────

void SourceFindDialog::requestSuggestion()
{
    if (m_busy)
        return;
    const int serial = ++m_suggestSerial;
    m_suggesting = true;
    m_statusLabel->setStyleSheet(QString());
    m_statusLabel->setText(u"Measuring the cube to suggest SoFiA-2 parameters…"_s);
    updateControls();

    const QJsonObject request =
            visivo::sofiaSuggestRequest(m_ask.pending.dataType, m_ask.pending.line);
    std::shared_ptr<BackendClient> client = m_client;   // shared with the worker
    const QString datasetId = m_datasetId;
    auto *watcher = new QFutureWatcher<BackendSofiaSuggestResult>(this);
    QPointer<SourceFindDialog> self(this);
    connect(watcher, &QFutureWatcher<BackendSofiaSuggestResult>::finished, watcher,
            [self, watcher, serial]() {
        const BackendSofiaSuggestResult answer = watcher->result();
        watcher->deleteLater();
        // A newer request (the user answered again) owns the state.
        if (!self || serial != self->m_suggestSerial)
            return;
        self->applySuggestion(answer);
    });
    watcher->setFuture(QtConcurrent::run([client, datasetId, request]() {
        return client->suggestSofia2Parameters(datasetId, request);
    }));
}

void SourceFindDialog::applySuggestion(const BackendSofiaSuggestResult &answer)
{
    m_suggesting = false;
    const auto showProblem = [this](const QString &text) {
        m_statusLabel->setStyleSheet(u"color: %1;"_s.arg(QString::fromLatin1(VisivoTheme::kError())));
        m_statusLabel->setText(text);
    };

    const visivo::SofiaSuggestion parsed = answer.httpStatus == 200
            ? visivo::parseSofiaSuggestion(answer.body) : visivo::SofiaSuggestion{};
    if (!parsed.valid) {
        if (answer.httpStatus == 422) {
            // The service's reason, as it gave it: not a cube, Stokes Q/U/V,
            // no spectral axis, too few channels, a compressed or extension
            // HDU. SoFiA-2 would not search it meaningfully either.
            m_dataRefused = true;
            m_suggestError = answer.error;
            showProblem(u"No SoFiA-2 run for this dataset: %1"_s.arg(answer.error));
        } else if (answer.httpStatus == 429) {
            m_suggestError = u"the backend was busy"_s;
            showProblem(u"The backend is busy: every compute slot of this session is in use. "
                        u"Press Suggest again when the running computation has finished."_s);
        } else if (answer.httpStatus == 404 && answer.error == u"Not Found"_s) {
            m_suggestError = u"this backend predates the suggester"_s;
            showProblem(u"This backend does not suggest SoFiA-2 parameters (it predates the "
                        u"suggester). A run uses SoFiA-2's defaults for everything not in the table."_s);
        } else {
            m_suggestError = answer.httpStatus == 200 ? parsed.error : answer.error;
            if (m_suggestError.isEmpty())
                m_suggestError = u"no answer"_s;
            showProblem(u"No suggestion: %1"_s.arg(m_suggestError));
        }
        // The table keeps the last good suggestion, so the answers shown, and
        // asked next, go back to the ones it was made for.
        m_ask.failed();
        if (m_suggestion.valid) {
            showChoice(m_dataTypeCombo, m_dataTypeNote, m_dataTypeConfirm, m_suggestion.dataType,
                       u"data type"_s);
            showChoice(m_lineCombo, m_lineNote, m_lineConfirm, m_suggestion.line, u"line widths"_s);
        }
        updateControls();
        return;
    }

    m_ask.succeeded();
    m_suggestion = parsed;
    m_suggestError.clear();
    m_dataRefused = false;
    showChoice(m_dataTypeCombo, m_dataTypeNote, m_dataTypeConfirm, parsed.dataType, u"data type"_s);
    showChoice(m_lineCombo, m_lineNote, m_lineConfirm, parsed.line, u"line widths"_s);

    QString status = visivo::sofiaMeasurementLine(parsed.measurements);
    const QString channelNote = parsed.measurements.value(u"channel_note"_s).toString();
    if (!channelNote.isEmpty())
        status += u"\nChannel width read as: "_s + channelNote;
    if (!parsed.sofiaVersion.isEmpty())
        status += u"\nRules written for SoFiA-2 %1."_s.arg(parsed.sofiaVersion);
    m_statusLabel->setStyleSheet(ToolDialogStyle::helperCss());
    m_statusLabel->setText(status);

    QStringList warnings;
    for (const auto &w : parsed.warnings)
        warnings << u"• "_s + w.second;
    m_warningsLabel->setText(warnings.join(u'\n'));
    m_warningsLabel->setVisible(!warnings.isEmpty());

    m_params.setSofiaDefaults(parsed.sofiaDefaults);
    m_params.applySuggestion(parsed.parameters);
    fillTable();
    updateControls();
}

void SourceFindDialog::showChoice(QComboBox *combo, QLabel *note, QPushButton *confirm,
                                  const visivo::SofiaChoice &choice, const QString &what)
{
    combo->clear();
    for (const QString &option : choice.options)
        combo->addItem(optionLabel(option), option);
    const int index = combo->findData(choice.value);
    if (index >= 0)
        combo->setCurrentIndex(index);
    confirm->setVisible(choice.present && choice.ambiguous);
    if (!choice.present) {
        note->setText(u"Not reported by this backend."_s);
        return;
    }
    QString text = choice.reason;
    if (choice.confidence == u"user"_s)
        text = u"Your choice."_s;
    else if (!choice.confidence.isEmpty())
        text += u" (confidence: %1)"_s.arg(choice.confidence);
    if (choice.ambiguous) {
        // The question the suggester could not answer: the run waits for it.
        note->setStyleSheet(u"color: %1; font-size: 11px; padding-left: 2px;"_s.arg(
                QString::fromLatin1(VisivoTheme::kWarning())));
        note->setText(u"Please confirm the %1: %2"_s.arg(what, text));
    } else {
        note->setStyleSheet(ToolDialogStyle::helperCss());
        note->setText(text);
    }
}

void SourceFindDialog::fillTable()
{
    m_fillingTable = true;
    const QSignalBlocker blocker(m_paramTable);
    const auto &rows = m_params.rows();
    m_paramTable->setRowCount(static_cast<int>(rows.size()));
    for (int r = 0; r < rows.size(); ++r) {
        for (int c = 0; c < ColCount; ++c) {
            auto *item = new QTableWidgetItem();
            Qt::ItemFlags flags = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
            if (c == ColValue)
                flags |= Qt::ItemIsEditable;
            if (c == ColKey && rows[r].needsConfirmation)
                flags |= Qt::ItemIsUserCheckable;
            item->setFlags(flags);
            m_paramTable->setItem(r, c, item);
        }
        updateRow(r);
    }
    m_paramTable->resizeColumnsToContents();
    // Short columns stay short (a long status or default is in the tooltip);
    // the reason is prose: the last column stretches over what is left of
    // the dialog's width and wraps, rather than pushing past its edge.
    for (int c = 0; c < ColReason; ++c)
        m_paramTable->setColumnWidth(c, std::min(m_paramTable->columnWidth(c), 220));
    m_paramTable->setColumnWidth(ColReason, 260);
    m_paramTable->horizontalHeader()->setStretchLastSection(true);
    m_paramTable->resizeRowsToContents();
    m_fillingTable = false;
}

void SourceFindDialog::updateRow(int row)
{
    const auto &rows = m_params.rows();
    if (row < 0 || row >= rows.size())
        return;
    const visivo::SofiaParamSet::Row &r = rows[row];
    const bool wasFilling = m_fillingTable;
    m_fillingTable = true;
    const QSignalBlocker blocker(m_paramTable);

    QTableWidgetItem *key = m_paramTable->item(row, ColKey);
    QTableWidgetItem *value = m_paramTable->item(row, ColValue);
    QTableWidgetItem *status = m_paramTable->item(row, ColStatus);
    QTableWidgetItem *def = m_paramTable->item(row, ColDefault);
    QTableWidgetItem *reason = m_paramTable->item(row, ColReason);
    if (!key || !value || !status || !def || !reason) {
        m_fillingTable = wasFilling;
        return;
    }
    key->setText(r.key);
    if (r.needsConfirmation) {
        key->setCheckState(r.confirmed ? Qt::Checked : Qt::Unchecked);
        key->setToolTip(u"A proposal: it is sent only when ticked."_s);
    }
    value->setText(r.value);
    QFont valueFont = value->font();
    valueFont.setBold(visivo::SofiaParamSet::differsFromDefault(r));
    valueFont.setItalic(r.edited);
    value->setFont(valueFont);
    value->setToolTip(r.suggested.isEmpty() ? u"Set by you."_s
                                            : u"Suggested: %1"_s.arg(r.suggested));
    status->setText(visivo::SofiaParamSet::rowStatus(r));
    status->setToolTip(status->text());
    if (r.edited || (r.needsConfirmation && !r.confirmed))
        status->setForeground(QColor(QString::fromLatin1(VisivoTheme::kWarning())));
    else
        status->setData(Qt::ForegroundRole, QVariant());
    def->setText(!r.defaultKnown ? u"unknown"_s : r.hasDefault ? r.sofiaDefault : u"none"_s);
    def->setToolTip(def->text());
    reason->setText(r.reason);
    reason->setToolTip(r.reason);
    m_fillingTable = wasFilling;
}

void SourceFindDialog::onCellChanged(QTableWidgetItem *item)
{
    if (m_fillingTable || !item)
        return;
    const int row = item->row();
    const auto &rows = m_params.rows();
    if (row < 0 || row >= rows.size())
        return;
    const QString key = rows[row].key;
    if (item->column() == ColKey && rows[row].needsConfirmation)
        m_params.setConfirmed(key, item->checkState() == Qt::Checked);
    else if (item->column() == ColValue)
        m_params.setValue(key, item->text());
    else
        return;
    // In place: the item that emitted this is still in QTableWidget::setData.
    updateRow(row);
    // "edited (suggested 0, 3, 7, 15)" is wider than anything the table was
    // sized for when it was filled.
    m_paramTable->resizeColumnToContents(ColStatus);
    updateControls();
}

void SourceFindDialog::onAddParameter()
{
    // The suggestion no longer sets dilation (the flux goal left v1), so this
    // is how a user who wants dilated masks gets them, or any other key.
    bool ok = false;
    const QString key = QInputDialog::getText(
            this, u"Add SoFiA-2 parameter"_s,
            u"SoFiA-2 2.7 parameter name (section.key), e.g. dilation.enable:"_s,
            QLineEdit::Normal, QString(), &ok).trimmed();
    if (!ok || key.isEmpty())
        return;
    if (m_params.indexOf(key) >= 0) {
        QMessageBox::information(this, u"Add SoFiA-2 parameter"_s,
                                 u"%1 is already in the table: edit its value there."_s.arg(key));
        return;
    }
    if (!m_params.acceptsKey(key)) {
        QMessageBox::warning(this, u"Add SoFiA-2 parameter"_s,
                             u"'%1' is not a SoFiA-2 2.7 parameter this backend accepts. The input "
                             u"and output settings are set by the backend."_s.arg(key));
        return;
    }
    const QString value = QInputDialog::getText(this, u"Add SoFiA-2 parameter"_s,
                                                u"Value of %1:"_s.arg(key), QLineEdit::Normal,
                                                QString(), &ok);
    if (!ok)
        return;
    if (!m_params.addUserParam(key, value)) {
        QMessageBox::warning(this, u"Add SoFiA-2 parameter"_s,
                             u"'%1' is not a SoFiA-2 parameter name (section.key)."_s.arg(key));
        return;
    }
    fillTable();
    updateControls();
}

// ── Run ─────────────────────────────────────────────────────────────────────

void SourceFindDialog::onRun()
{
    if (m_busy || m_suggesting)
        return;
    const QJsonObject params = m_params.runParams();

    // What the product records, as it is at the start: the parameters that
    // run and how each came to be, and the suggester's context.
    QVariantMap provenance = m_params.provenance();
    provenance.insert(u"Finder"_s, u"SoFiA-2 (task of the backend)"_s);
    int edited = 0;
    for (const auto &r : m_params.rows())
        edited += r.edited ? 1 : 0;
    if (m_suggestion.valid) {
        if (m_suggestion.dataType.present)
            provenance.insert(u"Data type"_s, choiceRecord(m_suggestion.dataType));
        if (m_suggestion.line.present)
            provenance.insert(u"Line widths"_s, choiceRecord(m_suggestion.line));
        provenance.insert(u"Parameters"_s,
                          edited == 0 ? u"as suggested from the data (rules for SoFiA-2 %1)"_s.arg(
                                                m_suggestion.sofiaVersion)
                                      : u"suggested from the data (rules for SoFiA-2 %1), %2 edited by you"_s
                                                .arg(m_suggestion.sofiaVersion)
                                                .arg(edited));
    } else {
        provenance.insert(u"Parameters"_s, u"SoFiA-2's defaults%1 (no suggestion: %2)"_s.arg(
                                                   params.isEmpty() ? QString() : u" and the table"_s,
                                                   m_suggestError.isEmpty() ? u"none asked"_s : m_suggestError));
    }

    m_cancel->store(false);
    m_busy = true;
    m_maskPath.clear();
    m_openMaskBtn->setEnabled(false);
    m_summaryLabel->clear();
    m_summaryLabel->setStyleSheet(QString());
    m_table->setRowCount(0);
    m_table->setColumnCount(0);
    m_table->setVisible(false);
    m_progressBar->setVisible(true);
    m_runClock.start();
    m_runLabel->setText(u"Starting SoFiA-2 on the backend…"_s);
    m_elapsedTimer->start();
    updateControls();

    // The log sink runs on the worker: dispatched through the application
    // object, which outlives the dialog, and checked there when it runs.
    QPointer<SourceFindDialog> self(this);
    auto log = [self](const QString &line) {
        QMetaObject::invokeMethod(qApp, [self, line]() {
            if (self && self->m_busy && line.startsWith(u"(poll)"_s))
                self->m_runLabel->setToolTip(line);   // a missed poll: no alarm, but findable
        }, Qt::QueuedConnection);
    };
    std::shared_ptr<BackendClient> client = m_client;
    const std::shared_ptr<std::atomic_bool> cancel = m_cancel;
    const QString datasetId = m_datasetId;
    auto *watcher = new QFutureWatcher<visivo::FinderTaskOutcome>(this);
    connect(watcher, &QFutureWatcher<visivo::FinderTaskOutcome>::finished, watcher,
            [self, watcher, params, provenance, edited]() {
        const visivo::FinderTaskOutcome outcome = watcher->result();
        watcher->deleteLater();
        if (self)
            self->showRunOutcome(outcome, params, provenance, edited);
    });
    watcher->setFuture(QtConcurrent::run([client, datasetId, params, cancel, log]() {
        return visivo::runFinderTask(*client, datasetId, u"sofia2"_s, params, cancel.get(), log);
    }));
}

void SourceFindDialog::onCancel()
{
    if (!m_busy)
        return;
    m_cancel->store(true);
    m_cancelBtn->setEnabled(false);
    m_runLabel->setText(u"Cancelling…"_s);
}

void SourceFindDialog::showRunOutcome(const visivo::FinderTaskOutcome &outcome,
                                      const QJsonObject &params, const QVariantMap &provenance,
                                      int edited)
{
    m_busy = false;
    m_elapsedTimer->stop();
    m_progressBar->setVisible(false);
    const QString took = elapsedText(m_runClock.elapsed());
    updateControls();

    if (!outcome.valid) {
        m_runLabel->setText(QString());
        m_summaryLabel->setStyleSheet(u"color: %1;"_s.arg(QString::fromLatin1(VisivoTheme::kError())));
        // In full: the backend sends SoFiA's ERROR lines and the end of its
        // output (up to 1200 characters), which is the diagnosis.
        if (outcome.cancelled) {
            m_summaryLabel->setStyleSheet(QString());
            m_summaryLabel->setPlainText(outcome.error == u"cancelled"_s
                                                 ? u"Cancelled: the backend accepted the cancellation "
                                                   u"and is stopping SoFiA-2."_s
                                                 : u"Cancelled: %1"_s.arg(outcome.error));
        } else if (outcome.refusedStatus == 503) {
            m_summaryLabel->setPlainText(
                    u"SoFiA-2 is not available on this backend: %1\nInstall SoFiA-2 where the "
                    u"backend runs (or point VISIVO_SOFIA_BIN at it) and try again."_s.arg(outcome.error));
        } else if (outcome.refusedStatus == 429) {
            m_summaryLabel->setPlainText(u"The backend is busy: %1"_s.arg(outcome.error));
        } else {
            m_summaryLabel->setPlainText(visivo::sofiaFailureText(outcome.error, outcome.refusedStatus));
        }
        return;
    }

    const QJsonObject &result = outcome.result;
    const int nSources = result.value(u"n_sources"_s).toInt();
    m_maskPath = result.value(u"mask_path"_s).toString();
    m_openMaskBtn->setEnabled(!m_maskPath.isEmpty());
    const visivo::SofiaRunSummary summary =
            visivo::parseSofiaRunSummary(result.value(u"summary"_s).toObject());
    const QString summaryText = visivo::sofiaRunSummaryText(summary);
    const int exitCode = visivo::sofiaExitCode(result);
    m_runLabel->setText(u"Finished in %1."_s.arg(took));
    // An empty result (SoFiA exit 8) is a result: said as such, with SoFiA's
    // counts, no mask to open, and recorded like any other run below.
    m_summaryLabel->setPlainText(visivo::sofiaRunHeadline(nSources, exitCode)
                                 + (summaryText.isEmpty() ? QString() : u"\n"_s + summaryText));
    const QJsonArray sources = result.value(u"sources"_s).toArray();
    populateSources(sources);
    if (!sources.isEmpty()) {
        // The sources now get their share of the height.
        m_table->setVisible(true);
        const int total = m_splitter->height();
        m_splitter->setSizes({ total * 55 / 100, total - total * 55 / 100 });
    }

    QVariantMap record = provenance;
    record.insert(u"Sources"_s, nSources);
    if (exitCode >= 0)
        record.insert(u"SoFiA exit code"_s, exitCode == 8 ? u"8 (no sources left after filtering)"_s
                                                           : QString::number(exitCode));
    if (!summaryText.isEmpty())
        record.insert(u"SoFiA summary"_s, summaryText);
    // "Output path" is what the Session Data row reopens: the mask cube.
    if (!m_maskPath.isEmpty())
        record.insert(u"Output path"_s, m_maskPath);
    const QString catalogPath = result.value(u"catalog_path"_s).toString();
    if (!catalogPath.isEmpty())
        record.insert(u"Catalogue path"_s, catalogPath);
    const QString logPath = result.value(u"log_path"_s).toString();
    if (!logPath.isEmpty())
        record.insert(u"Log path"_s, logPath);
    record.insert(u"Task"_s, outcome.taskId);
    // Two runs of the same parameters must still be two rows one can tell
    // apart in Session Data.
    const QString finishedAt = QTime::currentTime().toString(u"HH:mm:ss"_s);
    record.insert(u"Finished"_s, finishedAt);
    emit runRecorded(visivo::sofiaRunLabel(nSources, edited, finishedAt), record,
                     { visivo::sofiaSourceFindStep(m_datasetId, params) });
}

void SourceFindDialog::populateSources(const QJsonArray &sources)
{
    // Columns: the union of the rows' fields, so a catalogue with optional
    // columns still lines up; id and name first, then alphabetical.
    QStringList keys;
    for (const QJsonValue &v : sources)
        keys << v.toObject().keys();
    const QStringList columns = visivo::sofiaCatalogueColumns(keys);
    m_table->setColumnCount(static_cast<int>(columns.size()));
    m_table->setHorizontalHeaderLabels(columns);
    m_table->setRowCount(static_cast<int>(sources.size()));
    for (int r = 0; r < sources.size(); ++r) {
        const QJsonObject row = sources.at(r).toObject();
        for (int c = 0; c < columns.size(); ++c) {
            auto *item = new QTableWidgetItem(row.value(columns[c]).toVariant().toString());
            item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            m_table->setItem(r, c, item);
        }
    }
    m_table->resizeColumnsToContents();
}

// ── State ───────────────────────────────────────────────────────────────────

void SourceFindDialog::updateControls()
{
    const bool idle = !m_busy && !m_suggesting;
    const bool haveSuggestion = m_suggestion.valid;
    const bool open = idle && !m_dataRefused;
    m_addBtn->setEnabled(open);
    m_dataTypeCombo->setEnabled(open && haveSuggestion && m_suggestion.dataType.present);
    m_lineCombo->setEnabled(open && haveSuggestion && m_suggestion.line.present);
    m_dataTypeConfirm->setEnabled(open);
    m_lineConfirm->setEnabled(open);
    m_paramTable->setEnabled(open);
    m_resetBtn->setEnabled(open && m_params.anyEdited());
    m_suggestBtn->setEnabled(open);

    const bool unanswered = haveSuggestion
            && ((m_suggestion.dataType.present && m_suggestion.dataType.ambiguous)
                || (m_suggestion.line.present && m_suggestion.line.ambiguous));
    m_runBtn->setEnabled(open && !unanswered);
    if (m_dataRefused)
        m_runBtn->setToolTip(u"The backend refused this dataset: see the reason above."_s);
    else if (unanswered)
        m_runBtn->setToolTip(u"Answer the question above first (data type or line widths)."_s);
    else if (!haveSuggestion)
        m_runBtn->setToolTip(u"Runs with SoFiA-2's defaults for everything not in the table."_s);
    else if (m_params.hasUnconfirmed())
        m_runBtn->setToolTip(u"Runs on the whole cube: the proposed sub-cube is not ticked."_s);
    else
        m_runBtn->setToolTip(u"Run SoFiA-2 with the parameters in the table."_s);
    m_cancelBtn->setEnabled(m_busy && !m_cancel->load());
}

void SourceFindDialog::closeEvent(QCloseEvent *event)
{
    if (m_busy) {
        // Unlike the synchronous tools (ToolDialogClose.h), this run can be
        // stopped: closing cancels the task on the backend.
        const auto answer = QMessageBox::question(
                this, u"SoFiA-2 running"_s,
                u"SoFiA-2 is still running on the backend. Cancel the run and close?"_s,
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes) {
            event->ignore();
            return;
        }
        m_cancel->store(true);
    }
    event->accept();
}
