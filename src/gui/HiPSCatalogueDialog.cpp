#include "HiPSCatalogueDialog.h"

#include "RemoteFileBrowserDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <cmath>

using namespace Qt::StringLiterals;

namespace {

QString formatCentre(double raDeg, double decDeg)
{
    return QStringLiteral("RA %1°   Dec %2°")
            .arg(raDeg, 0, 'f', 5)
            .arg(decDeg, 0, 'f', 5);
}

} // namespace

HiPSCatalogueDialog::HiPSCatalogueDialog(const QString &backendUrl, const QString &backendToken,
                                         double raDeg, double decDeg, double fovDeg,
                                         QWidget *parent)
    : QDialog(parent)
    , m_backendUrl(backendUrl)
    , m_backendToken(backendToken)
    , m_raDeg(raDeg)
    , m_decDeg(decDeg)
{
    setWindowTitle(u"Overlay a Catalogue"_s);
    resize(620, 380);

    auto *root = new QVBoxLayout(this);

    // ── VO cone search ────────────────────────────────────────────────────
    m_coneRadio = new QRadioButton(u"Search a VO service over this field"_s, this);
    m_coneRadio->setChecked(true);
    root->addWidget(m_coneRadio);

    auto *coneBox = new QGroupBox(this);
    auto *coneForm = new QFormLayout(coneBox);
    // macOS defaults to FieldsStayAtSizeHint, which left the service URL
    // showing only its last few characters.
    coneForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    m_serviceCombo = new QComboBox(coneBox);
    m_serviceCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_serviceCombo->setMinimumContentsLength(24);
    coneForm->addRow(u"Service:"_s, m_serviceCombo);

    m_serviceUrlEdit = new QLineEdit(coneBox);
    m_serviceUrlEdit->setPlaceholderText(u"https://…/conesearch/…"_s);
    m_serviceUrlEdit->setToolTip(u"The cone-search endpoint. Pick one above, or paste another."_s);
    coneForm->addRow(u"URL:"_s, m_serviceUrlEdit);

    m_centreLabel = new QLabel(formatCentre(raDeg, decDeg), coneBox);
    coneForm->addRow(u"Centre:"_s, m_centreLabel);

    m_radiusSpin = new QDoubleSpinBox(coneBox);
    m_radiusSpin->setRange(0.001, 90.0);
    m_radiusSpin->setDecimals(4);
    m_radiusSpin->setSuffix(u" °"_s);
    // Half the field of view is the field on screen; a whole-sky view would ask
    // the service for the whole sky, which no catalogue service will answer, so
    // it is capped at something a cone search can actually serve.
    m_radiusSpin->setValue(std::clamp(fovDeg * 0.5, 0.001, 5.0));
    m_radiusSpin->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    coneForm->addRow(u"Radius:"_s, m_radiusSpin);
    if (fovDeg * 0.5 > 5.0) {
        auto *note = new QLabel(
                QStringLiteral("The field is %1° across; the radius starts at 5°, so this "
                               "covers the centre only. Service limits vary.")
                        .arg(fovDeg, 0, 'f', 1), coneBox);
        note->setWordWrap(true);
        coneForm->addRow(QString(), note);
    }

    root->addWidget(coneBox);

    // ── A file already on the backend ─────────────────────────────────────
    m_fileRadio = new QRadioButton(u"Use a catalogue file on the backend"_s, this);
    root->addWidget(m_fileRadio);

    auto *fileBox = new QGroupBox(this);
    auto *fileRow = new QHBoxLayout(fileBox);
    m_pathEdit = new QLineEdit(fileBox);
    m_pathEdit->setPlaceholderText(u"/data/catalogue.csv"_s);
    fileRow->addWidget(m_pathEdit, 1);
    m_browseButton = new QPushButton(u"Browse…"_s, fileBox);
    QObject::connect(m_browseButton, &QPushButton::clicked,
                     this, &HiPSCatalogueDialog::onBrowseFile);
    fileRow->addWidget(m_browseButton);
    root->addWidget(fileBox);

    m_statusLabel = new QLabel(this);
    m_statusLabel->setWordWrap(true);
    root->addWidget(m_statusLabel);
    root->addStretch(1);

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    QObject::connect(m_buttons, &QDialogButtonBox::accepted,
                     this, &HiPSCatalogueDialog::onAccept);
    QObject::connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(m_buttons);

    QObject::connect(m_coneRadio, &QRadioButton::toggled,
                     this, &HiPSCatalogueDialog::onModeChanged);
    QObject::connect(m_serviceCombo, &QComboBox::currentIndexChanged,
                     this, &HiPSCatalogueDialog::onServiceChanged);
    QObject::connect(&m_searchWatcher, &QFutureWatcher<BackendConeSearchResult>::finished,
                     this, &HiPSCatalogueDialog::onSearchFinished);

    // The known endpoints come from the backend, so correcting one does not
    // need a new client — but fetching them must not hold up the dialog:
    // asking for them before exec() left a slow backend showing nothing at
    // all, not even a Cancel button, and pumped a nested event loop while it
    // waited.
    m_serviceCombo->addItem(u"Loading services…"_s, QString());
    QObject::connect(&m_servicesWatcher,
                     &QFutureWatcher<QList<BackendConeSearchService>>::finished,
                     this, &HiPSCatalogueDialog::onServicesLoaded);
    const QString backend = m_backendUrl, tok = m_backendToken;
    m_servicesWatcher.setFuture(QtConcurrent::run([backend, tok]() {
        BackendClient client(backend, tok);
        return client.coneSearchServices();
    }));

    onModeChanged();
}

void HiPSCatalogueDialog::onServicesLoaded()
{
    m_services = m_servicesWatcher.result();
    m_servicesLoading = false;
    if (!m_serviceCombo) { updateControls(); return; }

    const QString typed = m_serviceUrlEdit ? m_serviceUrlEdit->text().trimmed() : QString();

    const QSignalBlocker blocker(m_serviceCombo);
    m_serviceCombo->clear();
    for (const auto &service : std::as_const(m_services))
        m_serviceCombo->addItem(service.name, service.url);
    m_serviceCombo->addItem(u"Other (type a URL below)"_s, QString());

    if (typed.isEmpty()) {
        m_serviceCombo->setCurrentIndex(0);
        if (!m_services.isEmpty() && m_serviceUrlEdit)
            m_serviceUrlEdit->setText(m_services.first().url);
    } else {
        // A URL typed while the list was loading stays, and the combo is moved
        // to match it — or to "Other", rather than pointing at a service the
        // search is not going to use.
        const int match = m_serviceCombo->findData(typed);
        m_serviceCombo->setCurrentIndex(match >= 0 ? match : m_serviceCombo->count() - 1);
    }
    updateControls();
}

QString HiPSCatalogueDialog::serviceNameFor(const QString &url) const
{
    for (const auto &service : std::as_const(m_services)) {
        if (service.url == url)
            return service.name;
    }
    return url;
}

void HiPSCatalogueDialog::onServiceChanged(int index)
{
    if (!m_serviceCombo || !m_serviceUrlEdit || index < 0) return;
    const QString url = m_serviceCombo->itemData(index).toString();
    if (!url.isEmpty())
        m_serviceUrlEdit->setText(url);
}

void HiPSCatalogueDialog::onModeChanged()
{
    updateControls();
}

void HiPSCatalogueDialog::updateControls()
{
    const bool cone = m_coneRadio && m_coneRadio->isChecked();
    const bool idle = !m_searching;

    if (m_coneRadio)      m_coneRadio->setEnabled(idle);
    if (m_fileRadio)      m_fileRadio->setEnabled(idle);
    if (m_serviceCombo)   m_serviceCombo->setEnabled(cone && idle && !m_servicesLoading);
    if (m_serviceUrlEdit) m_serviceUrlEdit->setEnabled(cone && idle);
    if (m_radiusSpin)     m_radiusSpin->setEnabled(cone && idle);
    if (m_pathEdit)       m_pathEdit->setEnabled(!cone && idle);
    if (m_browseButton)   m_browseButton->setEnabled(!cone && idle);
    if (m_buttons)        m_buttons->button(QDialogButtonBox::Ok)->setEnabled(idle);
}

void HiPSCatalogueDialog::onBrowseFile()
{
    RemoteFileBrowserDialog browser(m_backendUrl, m_backendToken, this);
    browser.setExtensionFilter({ u".csv"_s, u".tsv"_s, u".txt"_s }, u"Catalogues (CSV)"_s);
    if (browser.exec() != QDialog::Accepted) return;
    const QString path = browser.selectedFilePath();
    if (path.isEmpty()) return;
    if (m_pathEdit) m_pathEdit->setText(path);
    if (m_fileRadio) m_fileRadio->setChecked(true);
}

void HiPSCatalogueDialog::onAccept()
{
    if (m_fileRadio && m_fileRadio->isChecked()) {
        const QString path = m_pathEdit ? m_pathEdit->text().trimmed() : QString();
        if (path.isEmpty()) {
            if (m_statusLabel) m_statusLabel->setText(u"Choose a catalogue file first."_s);
            return;
        }
        m_cataloguePath = path;
        m_description = path;
        accept();
        return;
    }

    const QString serviceUrl = m_serviceUrlEdit ? m_serviceUrlEdit->text().trimmed() : QString();
    if (serviceUrl.isEmpty()) {
        if (m_statusLabel) m_statusLabel->setText(u"Enter the URL of a cone-search service."_s);
        return;
    }
    if (m_searchWatcher.isRunning()) return;

    if (m_statusLabel) m_statusLabel->setText(u"Searching…"_s);
    // Freeze the inputs for the duration, and name the service from the URL
    // actually submitted: the combo's current text would describe the result
    // with whatever was selected when it came back, or with "Loading services…"
    // for a URL typed before the list arrived.
    m_searchedService = serviceNameFor(serviceUrl);
    m_searching = true;
    updateControls();

    const QString url = m_backendUrl, token = m_backendToken;
    const double ra = m_raDeg, dec = m_decDeg;
    const double radius = m_radiusSpin ? m_radiusSpin->value() : 0.5;
    m_searchWatcher.setFuture(QtConcurrent::run([url, token, serviceUrl, ra, dec, radius]() {
        BackendClient client(url, token);
        return client.coneSearch(serviceUrl, ra, dec, radius, 2, u"hips_overlay.csv"_s);
    }));
}

void HiPSCatalogueDialog::onSearchFinished()
{
    const BackendConeSearchResult result = m_searchWatcher.result();
    m_searching = false;
    updateControls();

    if (!result.valid) {
        if (m_statusLabel)
            m_statusLabel->setText(result.error.isEmpty()
                                           ? u"The search failed."_s
                                           : result.error);
        return;
    }
    if (result.outputPath.isEmpty()) {
        // A valid answer with nothing in it: say so rather than overlaying an
        // empty file and leaving the user to wonder.
        if (m_statusLabel)
            m_statusLabel->setText(u"The service returned no sources in this field."_s);
        return;
    }

    m_cataloguePath = result.outputPath;
    m_raField  = result.raColumn;
    m_decField = result.decColumn;
    m_description = QStringLiteral("%1 source%2 from %3")
                            .arg(result.totalRows)
                            .arg(result.totalRows == 1 ? QString() : u"s"_s)
                            .arg(m_searchedService);
    accept();
}
