#include "SkavaSearchPanel.h"

#include "../Settings.h"
#include "../app/BackendClient.h"

#include <QAbstractItemModel>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QDoubleValidator>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIntValidator>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QTableView>
#include <QVBoxLayout>
#include <QtConcurrentRun>

#include <cmath>
#include <limits>

using namespace Qt::StringLiterals;

namespace {

constexpr double kSpeedOfLightMps = 299'792'458.0;

/// Wavelength (m) → frequency (Hz). Returns NaN if inputs invalid.
double wavelengthMtoHz(double m)
{
    return (m > 0.0 && std::isfinite(m)) ? kSpeedOfLightMps / m
                                           : std::numeric_limits<double>::quiet_NaN();
}

double frequencyHzToWavelengthM(double hz)
{
    return (hz > 0.0 && std::isfinite(hz)) ? kSpeedOfLightMps / hz
                                              : std::numeric_limits<double>::quiet_NaN();
}

/// Convert a frequency value entered in the UI to Hz, given the current
/// combo-box unit. Recognised: "Hz", "MHz", "GHz", "m".
double unitToHz(double value, const QString &unit)
{
    const QString u = unit.trimmed().toLower();
    if (u == "hz"_L1)   return value;
    if (u == "khz"_L1)  return value * 1e3;
    if (u == "mhz"_L1)  return value * 1e6;
    if (u == "ghz"_L1)  return value * 1e9;
    if (u == "m"_L1)    return wavelengthMtoHz(value);
    return value;
}

/// Format an em_min..em_max range (metres) into a human label.
/// Radio data show up as MHz/GHz, mid-IR as μm, etc.
QString formatEmRange(double emMin_m, double emMax_m)
{
    if (!std::isfinite(emMin_m) || !std::isfinite(emMax_m)) return {};
    const double fHi = frequencyHzToWavelengthM(emMin_m);  // shorter wavelength → higher freq
    const double fLo = frequencyHzToWavelengthM(emMax_m);
    if (!std::isfinite(fHi) || !std::isfinite(fLo)) return {};
    // Pick the unit that gives the most readable value
    const double mid = 0.5 * (fHi + fLo);
    if (mid >= 1e9) {
        return QStringLiteral("%1–%2 GHz").arg(fLo / 1e9, 0, 'f', 3)
                                           .arg(fHi / 1e9, 0, 'f', 3);
    } else if (mid >= 1e6) {
        return QStringLiteral("%1–%2 MHz").arg(fLo / 1e6, 0, 'f', 2)
                                           .arg(fHi / 1e6, 0, 'f', 2);
    } else if (emMin_m < 1e-4) {
        return QStringLiteral("%1–%2 μm").arg(emMin_m * 1e6, 0, 'f', 2)
                                          .arg(emMax_m * 1e6, 0, 'f', 2);
    }
    return QStringLiteral("%1–%2 m").arg(emMin_m, 0, 'g', 4)
                                     .arg(emMax_m, 0, 'g', 4);
}

QString formatPos(double ra, double dec)
{
    if (!std::isfinite(ra) || !std::isfinite(dec)) return {};
    return QStringLiteral("%1°, %2°").arg(ra, 0, 'f', 4).arg(dec, 0, 'f', 4);
}

QString formatMjdRange(double tMin, double tMax)
{
    if (!std::isfinite(tMin) || !std::isfinite(tMax)) return {};
    return QStringLiteral("MJD %1–%2").arg(tMin, 0, 'f', 2).arg(tMax, 0, 'f', 2);
}

}  // namespace

// ── Results model ─────────────────────────────────────────────────────────

class SkavaResultsModel : public QAbstractTableModel
{
    Q_OBJECT
public:
    enum Column {
        ColObsId = 0,
        ColTarget,
        ColType,
        ColPos,
        ColBand,
        ColTime,
        ColBestNode,
        ColLatency,
        ColCount,
    };

    explicit SkavaResultsModel(QObject *parent = nullptr)
        : QAbstractTableModel(parent) {}

    void setResults(const QVector<SkavaDiscoveryResult> &results)
    {
        beginResetModel();
        m_rows = results;
        endResetModel();
    }
    const QVector<SkavaDiscoveryResult> &results() const { return m_rows; }

    int rowCount(const QModelIndex & = {}) const override { return m_rows.size(); }
    int columnCount(const QModelIndex & = {}) const override { return ColCount; }

    QVariant data(const QModelIndex &index, int role) const override
    {
        if (!index.isValid()
            || index.row() < 0 || index.row() >= m_rows.size())
            return {};
        const auto &r = m_rows[index.row()];
        if (role == Qt::DisplayRole) {
            switch (index.column()) {
            case ColObsId:    return r.metadata.obsId;
            case ColTarget:   return r.metadata.targetName;
            case ColType:     return r.metadata.dataproductType;
            case ColPos:      return formatPos(r.metadata.sRa, r.metadata.sDec);
            case ColBand:     return formatEmRange(r.metadata.emMin, r.metadata.emMax);
            case ColTime:     return formatMjdRange(r.metadata.tMin, r.metadata.tMax);
            case ColBestNode: return r.bestNode;
            case ColLatency: {
                if (r.replicas.isEmpty()) return {};
                // Show the best replica's latency_score.
                double best = std::numeric_limits<double>::infinity();
                for (const auto &rep : r.replicas) {
                    if (rep.latencyScore < best) best = rep.latencyScore;
                }
                if (!std::isfinite(best)) return {};
                return QStringLiteral("%1 ms").arg(best, 0, 'f', 1);
            }
            default: return {};
            }
        }
        if (role == Qt::ToolTipRole && index.column() == ColObsId) {
            return r.metadata.obsTitle;
        }
        return {};
    }

    QVariant headerData(int section, Qt::Orientation o, int role) const override
    {
        if (o != Qt::Horizontal || role != Qt::DisplayRole) return {};
        switch (section) {
        case ColObsId:    return u"obs_id"_s;
        case ColTarget:   return u"Target"_s;
        case ColType:     return u"Type"_s;
        case ColPos:      return u"Sky position"_s;
        case ColBand:     return u"Band"_s;
        case ColTime:     return u"Time"_s;
        case ColBestNode: return u"Best node"_s;
        case ColLatency:  return u"Latency"_s;
        default:          return {};
        }
    }

private:
    QVector<SkavaDiscoveryResult> m_rows;
};


// ── Constructor / build UI ─────────────────────────────────────────────────

SkavaSearchPanel::SkavaSearchPanel(Settings *settings,
                                    BackendClient *backendClient,
                                    QWidget *parent)
    : QWidget(parent),
      m_settings(settings),
      m_backendClient(backendClient),
      m_skavaClient(new SkavaClient(QString(), QString(), this)),
      m_resultsModel(new SkavaResultsModel(this)),
      m_searchWatcher(new QFutureWatcher<SkavaDiscoveryResponse>(this)),
      m_datalinkWatcher(new QFutureWatcher<SkavaDatalinkResponse>(this))
{
    buildUi();
    applySettingsToFields();
    reloadClientFromFields();

    QObject::connect(m_searchWatcher, &QFutureWatcher<SkavaDiscoveryResponse>::finished,
                     this, [this]() {
        setBusy(false);
        const auto resp = m_searchWatcher->result();
        if (!resp.valid) {
            QMessageBox::warning(this, u"SKAVA Discovery"_s,
                    u"Search failed:\n\n%1"_s.arg(
                            resp.error.isEmpty() ? u"unknown error"_s : resp.error));
            return;
        }
        m_resultsModel->setResults(resp.results);
        m_statusLabel->setText(
                QStringLiteral("Showing %1 of %2 results (limit=%3, offset=%4)")
                        .arg(resp.returned).arg(resp.total)
                        .arg(resp.limit).arg(resp.offset));
        m_openBtn->setEnabled(false);
        if (m_subsetBtn) m_subsetBtn->setEnabled(false);
    });

    QObject::connect(m_datalinkWatcher, &QFutureWatcher<SkavaDatalinkResponse>::finished,
                     this, [this]() {
        setBusy(false);
        // Capture + clear the intent up front so a failed resolution can't
        // leave a stale subset flag that hijacks the next Open.
        const bool wantSubset = m_pendingSubset;
        m_pendingSubset = false;
        const auto resp = m_datalinkWatcher->result();
        if (!resp.valid) {
            QMessageBox::warning(this, u"SKAVA DataLink"_s,
                    u"Access resolution failed:\n\n%1"_s.arg(
                            resp.error.isEmpty() ? u"unknown error"_s : resp.error));
            return;
        }
        // The same DataLink resolution feeds two actions. When the user
        // clicked "SODA subset", route to the subset flow; otherwise do the
        // normal open.
        if (wantSubset) {
            emit sodaSubsetRequested(resp.dataset.obsId, resp);
            return;
        }
        // Backend-aware: emit the rich signal first so MainWindow can
        // pick a registered backend from the visivo-backend descriptors;
        // legacy signal is retained for any consumer that only wants the
        // URL. Both connect to the same effective flow inside MainWindow.
        emit openSkavaDatasetRequested(resp.dataset.obsId, resp);
        emit openDatasetUrlRequested(resp.primaryAccessUrl, resp.dataset.obsId);
    });

    QObject::connect(m_resultsTable->selectionModel(),
                     &QItemSelectionModel::currentRowChanged,
                     this, &SkavaSearchPanel::onResultSelectionChanged);
    QObject::connect(m_searchBtn, &QPushButton::clicked,
                     this, &SkavaSearchPanel::runSearch);
    QObject::connect(m_openBtn, &QPushButton::clicked,
                     this, &SkavaSearchPanel::openSelected);
    QObject::connect(m_subsetBtn, &QPushButton::clicked,
                     this, &SkavaSearchPanel::requestSubset);
    QObject::connect(m_copyCitationBtn, &QPushButton::clicked,
                     this, &SkavaSearchPanel::copyToClipboard);
    QObject::connect(m_urlEdit, &QLineEdit::editingFinished, this, [this]() {
        if (m_settings) m_settings->setSkavaUrl(m_urlEdit->text().trimmed());
        reloadClientFromFields();
    });
    QObject::connect(m_tokenEdit, &QLineEdit::editingFinished, this, [this]() {
        if (m_settings) m_settings->setSkavaToken(m_tokenEdit->text().trimmed());
        reloadClientFromFields();
    });
}

SkavaSearchPanel::~SkavaSearchPanel() = default;

void SkavaSearchPanel::buildUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(8);

    // ── Connection bar ───────────────────────────────────────────────
    {
        auto *box = new QGroupBox(u"Connection"_s, this);
        auto *form = new QFormLayout(box);
        m_urlEdit = new QLineEdit(box);
        m_urlEdit->setPlaceholderText(u"http://localhost:8000"_s);
        form->addRow(u"SKAVA URL"_s, m_urlEdit);
        m_tokenEdit = new QLineEdit(box);
        m_tokenEdit->setPlaceholderText(u"(optional bearer token)"_s);
        m_tokenEdit->setEchoMode(QLineEdit::Password);
        form->addRow(u"Token"_s, m_tokenEdit);
        root->addWidget(box);
    }

    // ── Scientific filters ───────────────────────────────────────────
    {
        auto *box = new QGroupBox(u"Scientific filters (ObsCore)"_s, this);
        auto *grid = new QGridLayout(box);
        grid->setHorizontalSpacing(8);
        grid->setVerticalSpacing(4);

        int row = 0;
        // POS
        grid->addWidget(new QLabel(u"POS, RA (deg)"_s), row, 0);
        m_raEdit = new QLineEdit; m_raEdit->setPlaceholderText(u"e.g. 187.5"_s);
        m_raEdit->setValidator(new QDoubleValidator(0.0, 360.0, 6, this));
        grid->addWidget(m_raEdit, row, 1);
        grid->addWidget(new QLabel(u"Dec (deg)"_s), row, 2);
        m_decEdit = new QLineEdit; m_decEdit->setPlaceholderText(u"e.g. 5.0"_s);
        m_decEdit->setValidator(new QDoubleValidator(-90.0, 90.0, 6, this));
        grid->addWidget(m_decEdit, row, 3);
        grid->addWidget(new QLabel(u"Radius (deg)"_s), row, 4);
        m_radiusEdit = new QLineEdit; m_radiusEdit->setPlaceholderText(u"e.g. 1.0"_s);
        m_radiusEdit->setValidator(new QDoubleValidator(0.0, 180.0, 6, this));
        grid->addWidget(m_radiusEdit, row, 5);
        ++row;

        // BAND
        grid->addWidget(new QLabel(u"BAND, min"_s), row, 0);
        m_freqMinEdit = new QLineEdit; m_freqMinEdit->setPlaceholderText(u"e.g. 900"_s);
        m_freqMinEdit->setValidator(new QDoubleValidator(0.0, 1e18, 6, this));
        grid->addWidget(m_freqMinEdit, row, 1);
        grid->addWidget(new QLabel(u"max"_s), row, 2);
        m_freqMaxEdit = new QLineEdit; m_freqMaxEdit->setPlaceholderText(u"e.g. 1670"_s);
        m_freqMaxEdit->setValidator(new QDoubleValidator(0.0, 1e18, 6, this));
        grid->addWidget(m_freqMaxEdit, row, 3);
        m_freqUnit = new QComboBox;
        m_freqUnit->addItems(QStringList{ u"MHz"_s, u"GHz"_s, u"Hz"_s, u"m"_s });
        grid->addWidget(m_freqUnit, row, 4);
        grid->addWidget(new QLabel(u"(min<max)"_s), row, 5);
        ++row;

        // TIME (MJD)
        grid->addWidget(new QLabel(u"TIME, MJD min"_s), row, 0);
        m_timeMinEdit = new QLineEdit; m_timeMinEdit->setPlaceholderText(u"e.g. 59000"_s);
        m_timeMinEdit->setValidator(new QDoubleValidator(0.0, 1e9, 4, this));
        grid->addWidget(m_timeMinEdit, row, 1);
        grid->addWidget(new QLabel(u"MJD max"_s), row, 2);
        m_timeMaxEdit = new QLineEdit; m_timeMaxEdit->setPlaceholderText(u"e.g. 60000"_s);
        m_timeMaxEdit->setValidator(new QDoubleValidator(0.0, 1e9, 4, this));
        grid->addWidget(m_timeMaxEdit, row, 3);
        ++row;

        // COLLECTION / DPTYPE / obs_id
        grid->addWidget(new QLabel(u"COLLECTION"_s), row, 0);
        m_collectionEdit = new QLineEdit;
        m_collectionEdit->setPlaceholderText(u"e.g. MGCLS"_s);
        grid->addWidget(m_collectionEdit, row, 1);
        grid->addWidget(new QLabel(u"DPTYPE"_s), row, 2);
        m_dptypeCombo = new QComboBox;
        m_dptypeCombo->addItems(QStringList{ u""_s, u"image"_s, u"cube"_s,
                                              u"timeseries"_s, u"spectrum"_s });
        grid->addWidget(m_dptypeCombo, row, 3);
        grid->addWidget(new QLabel(u"obs_id"_s), row, 4);
        m_obsIdEdit = new QLineEdit;
        m_obsIdEdit->setPlaceholderText(u"exact match"_s);
        grid->addWidget(m_obsIdEdit, row, 5);
        ++row;

        // Pagination
        grid->addWidget(new QLabel(u"limit"_s), row, 0);
        m_limitSpin = new QSpinBox;
        m_limitSpin->setRange(1, 1000);
        m_limitSpin->setValue(100);
        grid->addWidget(m_limitSpin, row, 1);
        grid->addWidget(new QLabel(u"offset"_s), row, 2);
        m_offsetSpin = new QSpinBox;
        m_offsetSpin->setRange(0, 1'000'000);
        m_offsetSpin->setValue(0);
        grid->addWidget(m_offsetSpin, row, 3);
        ++row;

        root->addWidget(box);
    }

    // ── Action bar ───────────────────────────────────────────────────
    {
        auto *bar = new QHBoxLayout;
        m_searchBtn = new QPushButton(u"Search"_s, this);
        m_searchBtn->setDefault(true);
        m_openBtn   = new QPushButton(u"Open selected in VisIVO"_s, this);
        m_openBtn->setEnabled(false);
        m_subsetBtn = new QPushButton(u"SODA subset…"_s, this);
        m_subsetBtn->setEnabled(false);
        m_subsetBtn->setToolTip(
                u"Compute a spatial/spectral cutout on the dataset's co-located "
                u"VisIVO backend (compute next to data) and open the result."_s);
        m_statusLabel = new QLabel(this);
        bar->addWidget(m_searchBtn);
        bar->addWidget(m_openBtn);
        bar->addWidget(m_subsetBtn);
        bar->addStretch(1);
        bar->addWidget(m_statusLabel);
        root->addLayout(bar);
    }

    // ── Results table ────────────────────────────────────────────────
    m_resultsTable = new QTableView(this);
    m_resultsTable->setModel(m_resultsModel);
    m_resultsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_resultsTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_resultsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_resultsTable->verticalHeader()->setVisible(false);
    m_resultsTable->horizontalHeader()->setStretchLastSection(true);
    m_resultsTable->setAlternatingRowColors(true);
    root->addWidget(m_resultsTable, 1);

    // ── Provenance ───────────────────────────────────────────────────
    {
        auto *box = new QGroupBox(u"Provenance"_s, this);
        auto *form = new QFormLayout(box);
        m_provenanceTitle = new QLabel(u"-"_s);
        m_provenanceTitle->setWordWrap(true);
        m_provenanceTitle->setTextInteractionFlags(Qt::TextSelectableByMouse);
        form->addRow(u"Title"_s, m_provenanceTitle);
        m_provenanceDoi = new QLabel(u"-"_s);
        m_provenanceDoi->setTextInteractionFlags(Qt::TextSelectableByMouse);
        m_provenanceDoi->setTextFormat(Qt::RichText);
        m_provenanceDoi->setOpenExternalLinks(true);
        form->addRow(u"DOI"_s, m_provenanceDoi);
        m_provenancePid = new QLabel(u"-"_s);
        m_provenancePid->setTextInteractionFlags(Qt::TextSelectableByMouse);
        form->addRow(u"PID"_s, m_provenancePid);
        m_provenanceLicense = new QLabel(u"-"_s);
        m_provenanceLicense->setTextInteractionFlags(Qt::TextSelectableByMouse);
        form->addRow(u"License"_s, m_provenanceLicense);
        m_provenanceCitation = new QLabel(u"-"_s);
        m_provenanceCitation->setWordWrap(true);
        m_provenanceCitation->setTextInteractionFlags(Qt::TextSelectableByMouse);
        form->addRow(u"Citation"_s, m_provenanceCitation);
        m_copyCitationBtn = new QPushButton(u"Copy citation"_s);
        m_copyCitationBtn->setEnabled(false);
        form->addRow(u""_s, m_copyCitationBtn);
        root->addWidget(box);
    }
}

void SkavaSearchPanel::applySettingsToFields()
{
    if (!m_settings) return;
    m_urlEdit->setText(m_settings->getSkavaUrl());
    m_tokenEdit->setText(m_settings->getSkavaToken());
}

void SkavaSearchPanel::reloadClientFromFields()
{
    if (!m_skavaClient) return;
    m_skavaClient->setBaseUrl(m_urlEdit->text().trimmed());
    m_skavaClient->setBearerToken(m_tokenEdit->text().trimmed());
}

void SkavaSearchPanel::setBusy(bool busy, const QString &message)
{
    m_searchBtn->setEnabled(!busy);
    const bool hasSelection = !busy
                              && m_resultsTable->currentIndex().isValid()
                              && m_resultsTable->currentIndex().row() >= 0;
    m_openBtn->setEnabled(hasSelection);
    if (m_subsetBtn) m_subsetBtn->setEnabled(hasSelection);
    if (busy) {
        m_statusLabel->setText(message.isEmpty() ? u"Working…"_s : message);
    }
    // The override-cursor stack must stay balanced: one push per busy=true,
    // exactly one pop per busy=false. The previous version pushed an Arrow
    // cursor on busy=false and then popped, which left the original Wait
    // override on the stack and made the cursor "stick".
    if (busy && !m_cursorOverrideActive) {
        QApplication::setOverrideCursor(Qt::WaitCursor);
        m_cursorOverrideActive = true;
    } else if (!busy && m_cursorOverrideActive) {
        QApplication::restoreOverrideCursor();
        m_cursorOverrideActive = false;
    }
}

void SkavaSearchPanel::runSearch()
{
    reloadClientFromFields();
    if (m_urlEdit->text().trimmed().isEmpty()) {
        QMessageBox::information(this, u"SKAVA"_s,
                u"Set the SKAVA base URL before searching."_s);
        return;
    }

    SkavaSearchFilter f;
    auto readDouble = [](QLineEdit *e) -> double {
        const QString t = e->text().trimmed();
        if (t.isEmpty()) return std::numeric_limits<double>::quiet_NaN();
        bool ok = false;
        const double v = t.toDouble(&ok);
        return ok ? v : std::numeric_limits<double>::quiet_NaN();
    };
    f.posRa     = readDouble(m_raEdit);
    f.posDec    = readDouble(m_decEdit);
    f.posRadius = readDouble(m_radiusEdit);
    // Convert frequency/wavelength range to metres for SKAVA.
    const double bandMinUser = readDouble(m_freqMinEdit);
    const double bandMaxUser = readDouble(m_freqMaxEdit);
    const QString unit = m_freqUnit->currentText();
    if (std::isfinite(bandMinUser) && std::isfinite(bandMaxUser)) {
        if (unit == "m"_L1) {
            f.bandMinMetres = std::min(bandMinUser, bandMaxUser);
            f.bandMaxMetres = std::max(bandMinUser, bandMaxUser);
        } else {
            // user gave frequencies → convert each to wavelength; the
            // *higher* frequency maps to the *smaller* wavelength.
            const double hzMin = unitToHz(bandMinUser, unit);
            const double hzMax = unitToHz(bandMaxUser, unit);
            const double lambdaLo = frequencyHzToWavelengthM(std::max(hzMin, hzMax));
            const double lambdaHi = frequencyHzToWavelengthM(std::min(hzMin, hzMax));
            f.bandMinMetres = lambdaLo;
            f.bandMaxMetres = lambdaHi;
        }
    }
    f.timeMinMjd = readDouble(m_timeMinEdit);
    f.timeMaxMjd = readDouble(m_timeMaxEdit);
    f.collection      = m_collectionEdit->text().trimmed();
    f.dataproductType = m_dptypeCombo->currentText().trimmed();
    f.obsId           = m_obsIdEdit->text().trimmed();
    f.limit  = m_limitSpin->value();
    f.offset = m_offsetSpin->value();

    setBusy(true, u"Searching…"_s);
    SkavaClient *client = m_skavaClient;  // captured by ptr
    m_searchWatcher->setFuture(QtConcurrent::run([client, f]() {
        return client->searchDiscovery(f);
    }));
}

void SkavaSearchPanel::onResultSelectionChanged()
{
    const int row = m_resultsTable->currentIndex().row();
    const auto &all = m_resultsModel->results();
    if (row < 0 || row >= all.size()) {
        m_openBtn->setEnabled(false);
        if (m_subsetBtn) m_subsetBtn->setEnabled(false);
        m_provenanceTitle->setText(u"-"_s);
        m_provenanceDoi->setText(u"-"_s);
        m_provenancePid->setText(u"-"_s);
        m_provenanceLicense->setText(u"-"_s);
        m_provenanceCitation->setText(u"-"_s);
        m_copyCitationBtn->setEnabled(false);
        return;
    }
    const auto &meta = all[row].metadata;
    m_openBtn->setEnabled(true);
    if (m_subsetBtn) m_subsetBtn->setEnabled(true);
    m_provenanceTitle->setText(
            meta.obsTitle.isEmpty() ? u"-"_s : meta.obsTitle);
    if (!meta.doi.isEmpty()) {
        const QString href = meta.doi.startsWith("http"_L1)
                ? meta.doi : (u"https://doi.org/"_s + meta.doi);
        m_provenanceDoi->setText(
                QStringLiteral("<a href='%1'>%2</a>").arg(href, meta.doi));
    } else {
        m_provenanceDoi->setText(u"-"_s);
    }
    m_provenancePid->setText(meta.pid.isEmpty() ? u"-"_s : meta.pid);
    m_provenanceLicense->setText(meta.license.isEmpty() ? u"-"_s : meta.license);
    m_provenanceCitation->setText(meta.citation.isEmpty() ? u"-"_s : meta.citation);
    m_copyCitationBtn->setEnabled(!meta.citation.isEmpty());
}

void SkavaSearchPanel::openSelected()
{
    reloadClientFromFields();
    const int row = m_resultsTable->currentIndex().row();
    const auto &all = m_resultsModel->results();
    if (row < 0 || row >= all.size()) return;
    const QString obsId = all[row].metadata.obsId;
    if (obsId.isEmpty()) return;

    setBusy(true, u"Resolving access URL…"_s);
    SkavaClient *client = m_skavaClient;
    m_datalinkWatcher->setFuture(QtConcurrent::run([client, obsId]() {
        return client->getDatalink(obsId);
    }));
}

void SkavaSearchPanel::requestSubset()
{
    reloadClientFromFields();
    const int row = m_resultsTable->currentIndex().row();
    const auto &all = m_resultsModel->results();
    if (row < 0 || row >= all.size()) return;
    const QString obsId = all[row].metadata.obsId;
    if (obsId.isEmpty()) return;

    // Reuse the shared DataLink resolution; the watcher routes to
    // sodaSubsetRequested instead of the open signals because of this flag.
    m_pendingSubset = true;
    setBusy(true, u"Resolving access for SODA subset…"_s);
    SkavaClient *client = m_skavaClient;
    m_datalinkWatcher->setFuture(QtConcurrent::run([client, obsId]() {
        return client->getDatalink(obsId);
    }));
}

void SkavaSearchPanel::copyToClipboard()
{
    const int row = m_resultsTable->currentIndex().row();
    const auto &all = m_resultsModel->results();
    if (row < 0 || row >= all.size()) return;
    QApplication::clipboard()->setText(all[row].metadata.citation);
    m_statusLabel->setText(u"Citation copied to clipboard."_s);
}

#include "SkavaSearchPanel.moc"
