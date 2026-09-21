// ConeSearchWindow.cpp — see ConeSearchWindow.h.

#include "ConeSearchWindow.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrent>

using namespace Qt::Literals::StringLiterals;

ConeSearchWindow::ConeSearchWindow(const QString &backendUrl, const QString &backendToken,
                                   QWidget *parent)
    : QWidget(parent, Qt::Window), m_backendUrl(backendUrl), m_backendToken(backendToken)
{
    setWindowTitle(u"Cone search"_s);
    setAttribute(Qt::WA_DeleteOnClose);
    resize(860, 560);
    buildUi();

    connect(&m_servicesWatcher, &QFutureWatcher<QList<BackendConeSearchService>>::finished, this,
            &ConeSearchWindow::onServicesFinished);
    connect(&m_searchWatcher, &QFutureWatcher<BackendConeSearchResult>::finished, this,
            &ConeSearchWindow::onSearchFinished);

    const QString url = m_backendUrl;
    const QString token = m_backendToken;
    m_servicesWatcher.setFuture(QtConcurrent::run([url, token]() {
        BackendClient client(url, token);
        return client.coneSearchServices();
    }));
}

ConeSearchWindow::~ConeSearchWindow()
{
    m_servicesWatcher.disconnect(this);
    m_searchWatcher.disconnect(this);
}

void ConeSearchWindow::buildUi()
{
    auto *layout = new QVBoxLayout(this);

    auto *row = new QHBoxLayout;
    row->addWidget(new QLabel(u"Service"_s, this));
    m_service = new QComboBox(this);
    m_service->setEditable(true);
    m_service->setToolTip(u"The base URL of a cone-search service. RA, Dec, SR and VERB "
                          u"are added to whatever query it already carries."_s);
    row->addWidget(m_service, 1);
    layout->addLayout(row);

    auto *position = new QHBoxLayout;
    const auto degrees = [this](double minimum, double maximum, double value, int decimals) {
        auto *box = new QDoubleSpinBox(this);
        box->setRange(minimum, maximum);
        box->setDecimals(decimals);
        box->setValue(value);
        box->setSuffix(u"°"_s);
        return box;
    };
    position->addWidget(new QLabel(u"RA"_s, this));
    m_ra = degrees(-360.0, 360.0, 0.0, 6);
    position->addWidget(m_ra);
    position->addWidget(new QLabel(u"Dec"_s, this));
    m_dec = degrees(-90.0, 90.0, 0.0, 6);
    position->addWidget(m_dec);
    position->addWidget(new QLabel(u"Radius"_s, this));
    m_radius = degrees(0.0001, 90.0, 0.05, 4);
    position->addWidget(m_radius);
    position->addWidget(new QLabel(u"Verbosity"_s, this));
    m_verbosity = new QComboBox(this);
    m_verbosity->addItems({ u"1 — minimal"_s, u"2 — standard"_s, u"3 — everything"_s });
    m_verbosity->setCurrentIndex(1);
    position->addWidget(m_verbosity);
    position->addStretch(1);
    m_searchButton = new QPushButton(u"Search"_s, this);
    m_searchButton->setDefault(true);
    connect(m_searchButton, &QPushButton::clicked, this, &ConeSearchWindow::search);
    position->addWidget(m_searchButton);
    layout->addLayout(position);

    m_table = new QTableWidget(0, 0, this);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setAlternatingRowColors(true);
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    layout->addWidget(m_table, 1);

    auto *footer = new QHBoxLayout;
    m_status = new QLabel(QString(), this);
    m_status->setWordWrap(true);
    footer->addWidget(m_status, 1);
    m_openButton = new QPushButton(u"Open as Catalogue"_s, this);
    m_openButton->setEnabled(false);
    m_openButton->setToolTip(u"Open the saved result in the 3-D catalogue viewer."_s);
    connect(m_openButton, &QPushButton::clicked, this, [this]() {
        if (!m_savedPath.isEmpty())
            emit this->catalogueReady(m_savedPath);
    });
    footer->addWidget(m_openButton);
    layout->addLayout(footer);
}

void ConeSearchWindow::setPosition(double ra, double dec, double radiusDeg)
{
    if (m_ra)
        m_ra->setValue(ra);
    if (m_dec)
        m_dec->setValue(dec);
    if (m_radius && radiusDeg > 0)
        m_radius->setValue(radiusDeg);
}

void ConeSearchWindow::onServicesFinished()
{
    for (const BackendConeSearchService &service : m_servicesWatcher.result())
        m_service->addItem(service.name, service.url);
    // The visible text is the name; what is sent is the URL behind it, unless
    // the user typed one.
    connect(m_service, &QComboBox::currentIndexChanged, this, [this](int index) {
        const QString url = m_service->itemData(index).toString();
        if (!url.isEmpty())
            m_service->setToolTip(url);
    });
}

void ConeSearchWindow::search()
{
    if (m_searchWatcher.isRunning())
        return;

    // The combo shows a name and carries the URL behind it, but it is editable:
    // a typed URL wins, and text that is neither is a mistake — using the URL
    // still attached to the index would search a service the user has just
    // typed over.
    const QString typed = m_service->currentText().trimmed();
    const int index = m_service->currentIndex();
    const bool untouched = index >= 0 && typed == m_service->itemText(index);
    QString serviceUrl = untouched ? m_service->itemData(index).toString() : QString();
    if (typed.startsWith(u"http://"_s) || typed.startsWith(u"https://"_s))
        serviceUrl = typed;
    if (serviceUrl.isEmpty()) {
        QMessageBox::warning(this, windowTitle(),
                             u"Choose a service from the list, or paste the full URL of "
                             u"a cone-search endpoint (http:// or https://)."_s);
        return;
    }

    m_searchButton->setEnabled(false);
    m_openButton->setEnabled(false);
    m_status->setText(u"Searching…"_s);

    const QString url = m_backendUrl;
    const QString token = m_backendToken;
    const double ra = m_ra->value();
    const double dec = m_dec->value();
    const double radius = m_radius->value();
    const int verbosity = m_verbosity->currentIndex() + 1;
    m_searchWatcher.setFuture(
            QtConcurrent::run([url, token, serviceUrl, ra, dec, radius, verbosity]() {
                BackendClient client(url, token);
                return client.coneSearch(serviceUrl, ra, dec, radius, verbosity);
            }));
}

void ConeSearchWindow::onSearchFinished()
{
    const BackendConeSearchResult result = m_searchWatcher.result();
    m_searchButton->setEnabled(true);
    if (!result.valid) {
        m_status->setText(result.error.isEmpty() ? u"The search failed."_s : result.error);
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
    m_openButton->setEnabled(!m_savedPath.isEmpty());

    QStringList notes;
    notes << u"%1 row(s)"_s.arg(result.totalRows);
    if (result.truncated)
        notes << u"showing the first %1"_s.arg(result.rows.size());
    if (!result.raColumn.isEmpty())
        notes << u"position from %1 / %2"_s.arg(result.raColumn, result.decColumn);
    if (!result.workspaceFilename.isEmpty())
        notes << u"saved as %1"_s.arg(result.workspaceFilename);
    m_status->setText(notes.join(u" · "_s));
}
