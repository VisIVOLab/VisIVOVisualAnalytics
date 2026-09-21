// VlkbQueryComposer.cpp — see VlkbQueryComposer.h.

#include "VlkbQueryComposer.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrent>

using namespace Qt::Literals::StringLiterals;

namespace {
/// The value carried by the "write your own" entry in the preset combo.
const QString kCustomPreset = QStringLiteral("__custom__");
}   // namespace

VlkbQueryComposer::VlkbQueryComposer(const QString &backendUrl, const QString &backendToken,
                                     QWidget *parent)
    : QDialog(parent), m_backendUrl(backendUrl), m_backendToken(backendToken)
{
    setWindowTitle(u"VLKB catalogue query"_s);
    resize(820, 600);
    buildUi();

    connect(&m_presetsWatcher, &QFutureWatcher<BackendVlkbPresetsResult>::finished, this,
            &VlkbQueryComposer::onPresetsFinished);
    connect(&m_queryWatcher, &QFutureWatcher<BackendVlkbTapResult>::finished, this,
            &VlkbQueryComposer::onQueryFinished);

    const QString url = m_backendUrl;
    const QString token = m_backendToken;
    m_presetsWatcher.setFuture(QtConcurrent::run([url, token]() {
        BackendClient client(url, token);
        return client.vlkbTapPresets();
    }));
}

VlkbQueryComposer::~VlkbQueryComposer()
{
    m_presetsWatcher.disconnect(this);
    m_queryWatcher.disconnect(this);
}

void VlkbQueryComposer::buildUi()
{
    auto *layout = new QVBoxLayout(this);
    auto *form = new QFormLayout;

    m_preset = new QComboBox(this);
    form->addRow(u"Catalogue"_s, m_preset);

    m_band = new QComboBox(this);
    form->addRow(u"Band"_s, m_band);

    const auto degrees = [this](double value) {
        auto *box = new QDoubleSpinBox(this);
        box->setRange(-360.0, 360.0);
        box->setDecimals(5);
        box->setValue(value);
        box->setSuffix(u"°"_s);
        return box;
    };
    m_glonMin = degrees(0.0);
    m_glonMax = degrees(1.0);
    m_glatMin = degrees(-1.0);
    m_glatMax = degrees(1.0);
    form->addRow(u"Galactic longitude"_s, m_glonMin);
    form->addRow(u"…to"_s, m_glonMax);
    form->addRow(u"Galactic latitude"_s, m_glatMin);
    form->addRow(u"…to"_s, m_glatMax);

    m_maxRows = new QSpinBox(this);
    m_maxRows->setRange(0, 1000000);
    m_maxRows->setValue(0);
    m_maxRows->setSpecialValueText(u"no limit"_s);
    m_maxRows->setToolTip(u"Passed to the service as MAXREC. The result says when it "
                          u"stopped exactly there, which usually means there is more."_s);
    form->addRow(u"Row limit"_s, m_maxRows);

    m_outputName = new QLineEdit(this);
    m_outputName->setPlaceholderText(u"named after the catalogue"_s);
    form->addRow(u"Save as"_s, m_outputName);

    layout->addLayout(form);

    m_adql = new QPlainTextEdit(this);
    m_adql->setPlaceholderText(u"SELECT * FROM compactsources.higal250 WHERE …"_s);
    m_adql->setMaximumHeight(110);
    m_adql->setVisible(false);
    layout->addWidget(m_adql);

    m_table = new QTableWidget(0, 0, this);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setAlternatingRowColors(true);
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    layout->addWidget(m_table, 1);

    m_status = new QLabel(u"Loading the catalogue list…"_s, this);
    m_status->setWordWrap(true);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_status);

    m_buttons = new QDialogButtonBox(this);
    auto *run = m_buttons->addButton(u"Run"_s, QDialogButtonBox::AcceptRole);
    run->setDefault(true);
    connect(run, &QPushButton::clicked, this, &VlkbQueryComposer::runQuery);
    m_openButton = m_buttons->addButton(u"Open as Catalogue"_s, QDialogButtonBox::ActionRole);
    m_openButton->setEnabled(false);
    connect(m_openButton, &QPushButton::clicked, this, [this]() {
        if (!m_savedPath.isEmpty())
            emit this->catalogueReady(m_savedPath);
    });
    m_buttons->addButton(QDialogButtonBox::Close);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(m_buttons);

    connect(m_preset, &QComboBox::currentIndexChanged, this,
            &VlkbQueryComposer::updatePresetFields);
}

void VlkbQueryComposer::setBox(double glonMin, double glonMax, double glatMin, double glatMax)
{
    if (m_glonMin && m_glonMax) {
        m_glonMin->setValue(std::min(glonMin, glonMax));
        m_glonMax->setValue(std::max(glonMin, glonMax));
    }
    if (m_glatMin && m_glatMax) {
        m_glatMin->setValue(std::min(glatMin, glatMax));
        m_glatMax->setValue(std::max(glatMin, glatMax));
    }
}

void VlkbQueryComposer::onPresetsFinished()
{
    const BackendVlkbPresetsResult presets = m_presetsWatcher.result();
    if (!presets.valid) {
        m_status->setText(presets.error);
        // The composer still works: an ADQL statement needs no preset list.
        m_preset->addItem(u"Write the query myself (ADQL)"_s, kCustomPreset);
        updatePresetFields();
        return;
    }
    for (const BackendVlkbPreset &preset : presets.presets)
        m_preset->addItem(preset.label, preset.id);
    m_preset->addItem(u"Write the query myself (ADQL)"_s, kCustomPreset);
    for (const QString &band : presets.bands)
        m_band->addItem(u"%1 µm"_s.arg(band), band);
    m_status->setText(presets.serviceUrl);
    updatePresetFields();
}

void VlkbQueryComposer::updatePresetFields()
{
    const QString preset = m_preset->currentData().toString();
    const bool custom = preset == kCustomPreset;
    // The band only means something for the per-band tables; the box means
    // nothing for a query the user wrote themselves.
    if (m_band)
        m_band->setEnabled(preset == u"band"_s);
    for (QWidget *w : { static_cast<QWidget *>(m_glonMin.data()),
                        static_cast<QWidget *>(m_glonMax.data()),
                        static_cast<QWidget *>(m_glatMin.data()),
                        static_cast<QWidget *>(m_glatMax.data()) }) {
        if (w)
            w->setEnabled(!custom);
    }
    if (m_adql)
        m_adql->setVisible(custom);
}

void VlkbQueryComposer::runQuery()
{
    if (m_queryWatcher.isRunning())
        return;
    const QString preset = m_preset->currentData().toString();
    const bool custom = preset == kCustomPreset;
    if (custom && m_adql->toPlainText().trimmed().isEmpty()) {
        QMessageBox::warning(this, windowTitle(), u"Write a query, or pick a catalogue."_s);
        return;
    }

    m_status->setText(u"Querying the VLKB…"_s);
    m_buttons->setEnabled(false);

    const QString url = m_backendUrl;
    const QString token = m_backendToken;
    const QString adql = custom ? m_adql->toPlainText().trimmed() : QString();
    const QString presetId = custom ? QString() : preset;
    const QString band = m_band->currentData().toString();
    const double glonMin = m_glonMin->value();
    const double glonMax = m_glonMax->value();
    const double glatMin = m_glatMin->value();
    const double glatMax = m_glatMax->value();
    const int maxRows = m_maxRows->value();
    const QString name = m_outputName->text().trimmed();
    m_queryWatcher.setFuture(QtConcurrent::run(
            [url, token, presetId, adql, glonMin, glonMax, glatMin, glatMax, band, maxRows,
             name]() {
                BackendClient client(url, token);
                return client.vlkbTapQuery(presetId, adql, glonMin, glonMax, glatMin, glatMax,
                                           band, maxRows, name);
            }));
}

void VlkbQueryComposer::onQueryFinished()
{
    const BackendVlkbTapResult result = m_queryWatcher.result();
    m_buttons->setEnabled(true);
    if (!result.valid) {
        m_status->setText(result.error.isEmpty() ? u"The query failed."_s : result.error);
        return;
    }

    m_table->clear();
    m_table->setRowCount(0);
    m_table->setColumnCount(result.columns.size());
    m_table->setHorizontalHeaderLabels(result.columns);
    for (const QStringList &row : result.rows) {
        const int index = m_table->rowCount();
        m_table->insertRow(index);
        for (int column = 0; column < row.size() && column < result.columns.size(); ++column)
            m_table->setItem(index, column, new QTableWidgetItem(row.at(column)));
    }

    m_savedPath = result.outputPath;
    if (m_openButton)
        m_openButton->setEnabled(!m_savedPath.isEmpty());

    QStringList notes;
    notes << u"%1 row(s)"_s.arg(result.totalRows);
    if (result.truncated)
        notes << u"showing the first %1"_s.arg(result.rows.size());
    if (result.hitRowLimit)
        notes << u"stopped at the row limit — there may be more"_s;
    if (!result.workspaceFilename.isEmpty())
        notes << u"saved as %1"_s.arg(result.workspaceFilename);
    else
        notes << u"nothing to save"_s;
    // The query itself: a preset is built on the backend, and this is the only
    // place the user can see what it asked for.
    m_status->setText(u"%1\n%2"_s.arg(notes.join(u" · "_s), result.query));
}
