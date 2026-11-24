#include "IsoLevelsDialog.h"

#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleValidator>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>

using namespace visivo::isolevels;

namespace {
constexpr int kValueColumn = 0;
constexpr int kColourColumn = 1;
constexpr int kOpacityColumn = 2;

QString swatchStyle(const QColor &c)
{
    return QStringLiteral("QPushButton { background-color: %1; border: 1px solid #888; "
                          "min-width: 44px; }")
            .arg(c.name());
}
}

IsoLevelsDialog::IsoLevelsDialog(const Setup &setup, QWidget *parent)
    : QDialog(parent)
    , m_setup(setup)
    , m_levels(setup.current)
{
    setWindowTitle(tr("Isosurface levels"));
    setMinimumWidth(460);
    auto *root = new QVBoxLayout(this);

    auto *intro = new QLabel(
            tr("Surfaces at several levels of the field, nested: the highest level is the "
               "innermost. Give a count between two values, or type the levels."),
            this);
    intro->setWordWrap(true);
    root->addWidget(intro);

    auto *form = new QFormLayout();
    auto *rangeRow = new QHBoxLayout();
    m_count = new QSpinBox(this);
    m_count->setRange(1, kMaxLevels);
    m_count->setValue(m_levels.isEmpty() ? 3 : static_cast<int>(m_levels.size()));
    m_count->setToolTip(tr("How many surfaces (at most %1).").arg(kMaxLevels));
    m_min = new QLineEdit(QString::number(m_levels.isEmpty() ? setup.range.min
                                                              : m_levels.first().value, 'g', 6),
                          this);
    m_max = new QLineEdit(QString::number(m_levels.isEmpty() ? setup.range.max
                                                              : m_levels.last().value, 'g', 6),
                          this);
    for (auto *e : { m_min.data(), m_max.data() })
        e->setValidator(new QDoubleValidator(e));
    m_spacing = new QComboBox(this);
    m_spacing->addItem(tr("Linear"), static_cast<int>(Spacing::Linear));
    m_spacing->addItem(tr("Log"), static_cast<int>(Spacing::Log));
    m_spacing->setCurrentIndex(setup.range.spacing == Spacing::Log ? 1 : 0);
    m_spacing->setToolTip(tr("Evenly spaced in the values, or in their logarithm (a field over "
                             "many decades, such as a simulation's density)."));
    auto *generateButton = new QPushButton(tr("Generate"), this);
    rangeRow->addWidget(m_count);
    rangeRow->addWidget(new QLabel(tr("levels from"), this));
    rangeRow->addWidget(m_min, 1);
    rangeRow->addWidget(new QLabel(tr("to"), this));
    rangeRow->addWidget(m_max, 1);
    rangeRow->addWidget(m_spacing);
    rangeRow->addWidget(generateButton);
    form->addRow(rangeRow);

    auto *listRow = new QHBoxLayout();
    m_list = new QLineEdit(this);
    m_list->setPlaceholderText(tr("or type the levels: 0.01, 0.1, 1"));
    auto *useList = new QPushButton(tr("Use list"), this);
    listRow->addWidget(m_list, 1);
    listRow->addWidget(useList);
    form->addRow(listRow);
    root->addLayout(form);

    m_table = new QTableWidget(0, 3, this);
    m_table->setHorizontalHeaderLabels(
            { setup.unit.isEmpty() ? tr("Level") : tr("Level (%1)").arg(setup.unit), tr("Colour"),
              tr("Opacity") });
    m_table->verticalHeader()->hide();
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->setSelectionMode(QAbstractItemView::NoSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setMinimumHeight(220);
    m_table->verticalHeader()->setDefaultSectionSize(34);   // room for the spin box's digits
    root->addWidget(m_table, 1);

    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    root->addWidget(m_status);

    auto *buttons = new QDialogButtonBox(this);
    auto *apply = buttons->addButton(tr("Apply"), QDialogButtonBox::ApplyRole);
    auto *single = buttons->addButton(tr("One surface at the threshold"),
                                      QDialogButtonBox::ActionRole);
    buttons->addButton(QDialogButtonBox::Close);
    root->addWidget(buttons);

    // Return in a field acts on that field only: no button is the dialog's default.
    for (QPushButton *b : { generateButton, useList, apply, single })
        b->setAutoDefault(false);
    if (auto *close = buttons->button(QDialogButtonBox::Close))
        close->setAutoDefault(false);
    connect(generateButton, &QPushButton::clicked, this, &IsoLevelsDialog::generate);
    connect(m_min, &QLineEdit::returnPressed, this, &IsoLevelsDialog::generate);
    connect(m_max, &QLineEdit::returnPressed, this, &IsoLevelsDialog::generate);
    connect(useList, &QPushButton::clicked, this, &IsoLevelsDialog::useTypedList);
    connect(m_list, &QLineEdit::returnPressed, this, &IsoLevelsDialog::useTypedList);
    connect(apply, &QPushButton::clicked, this, [this]() {
        if (m_levels.isEmpty()) {
            setStatus(tr("Generate or type the levels first."), true);
            return;
        }
        emit applied(m_levels);
    });
    connect(single, &QPushButton::clicked, this, [this]() {
        m_levels.clear();
        emit singleSurfaceRequested();
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    if (m_levels.isEmpty())
        generate();
    else
        fillTable();
}

void IsoLevelsDialog::setStatus(const QString &text, bool error)
{
    m_status->setStyleSheet(error ? QStringLiteral("color: #c0392b;") : QString());
    m_status->setText(text);
}

void IsoLevelsDialog::adopt(const QVector<double> &values)
{
    m_levels = keepEdits(makeLevels(values, m_setup.colourAt), m_levels);
    fillTable();
}

void IsoLevelsDialog::generate()
{
    QString error;
    const auto spacing = static_cast<Spacing>(m_spacing->currentData().toInt());
    const QVector<double> values =
            spaced(m_count->value(), QLocale::c().toDouble(m_min->text()),
                   QLocale::c().toDouble(m_max->text()), spacing, &error);
    if (values.isEmpty()) {
        setStatus(error, true);
        return;
    }
    adopt(values);
}

void IsoLevelsDialog::useTypedList()
{
    QString error;
    const QVector<double> values = parseList(m_list->text(), &error);
    if (values.isEmpty()) {
        setStatus(error, true);
        return;
    }
    m_count->setValue(static_cast<int>(values.size()));
    adopt(values);
}

void IsoLevelsDialog::fillTable()
{
    m_table->setRowCount(0);
    // Highest first, as the legend: the innermost surface at the top.
    for (int i = static_cast<int>(m_levels.size()) - 1; i >= 0; --i) {
        const int row = m_table->rowCount();
        m_table->insertRow(row);
        m_table->setItem(row, kValueColumn,
                         new QTableWidgetItem(label(m_levels[i].value)));
        auto *colour = new QPushButton(this);
        colour->setStyleSheet(swatchStyle(m_levels[i].colour));
        colour->setToolTip(tr("Pick this surface's colour."));
        colour->setAutoDefault(false);
        connect(colour, &QPushButton::clicked, this, [this, i, colour]() {
            const QColor picked = QColorDialog::getColor(m_levels[i].colour, this,
                                                         tr("Surface colour"));
            if (!picked.isValid())
                return;
            m_levels[i].colour = picked;
            colour->setStyleSheet(swatchStyle(picked));
        });
        m_table->setCellWidget(row, kColourColumn, colour);
        auto *opacity = new QDoubleSpinBox(this);
        opacity->setRange(0.05, 1.0);
        opacity->setSingleStep(0.05);
        opacity->setDecimals(2);
        opacity->setValue(m_levels[i].opacity);
        connect(opacity, &QDoubleSpinBox::valueChanged, this,
                [this, i](double v) { m_levels[i].opacity = v; });
        m_table->setCellWidget(row, kOpacityColumn, opacity);
    }
    setStatus(tr("%1 surface(s). Colours follow the colour map; opacity falls outward. "
                 "Edit either, then Apply.").arg(m_levels.size()),
              false);
}
