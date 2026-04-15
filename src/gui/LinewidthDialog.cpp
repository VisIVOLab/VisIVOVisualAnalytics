#include "LinewidthDialog.h"
#include "ScienceMapWindow.h"

#include "BackendClient.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QLabel>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <algorithm>
#include <cstring>

using namespace Qt::StringLiterals;

// ─────────────────────────────────────────────────────────────────────────────

LinewidthDialog::LinewidthDialog(BackendClient *client,
                                 const QString &sessionId,
                                 const QString &datasetId,
                                 int depth,
                                 double initialThreshold,
                                 QWidget *parent)
    : QDialog(parent)
    , m_client(client)
    , m_sessionId(sessionId)
    , m_datasetId(datasetId)
    , m_depth(std::max(1, depth))
{
    setWindowTitle(u"Line-Width Map"_s);
    setMinimumWidth(480);

    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(12, 12, 12, 12);
    mainLayout->setSpacing(8);

    auto *form = new QFormLayout();
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    form->setHorizontalSpacing(12);
    form->setVerticalSpacing(8);
    mainLayout->addLayout(form);

    // ── Method ───────────────────────────────────────────────────────────────
    m_methodCombo = new QComboBox(this);
    m_methodCombo->addItem(u"FWHM"_s,              u"fwhm"_s);
    m_methodCombo->addItem(u"Equivalent Width"_s,  u"ew"_s);
    form->addRow(u"Method"_s, m_methodCombo);

    // ── Rest frequency ────────────────────────────────────────────────────────
    m_restFreqSpin = new QDoubleSpinBox(this);
    m_restFreqSpin->setRange(0.0, 1000.0);
    m_restFreqSpin->setDecimals(6);
    m_restFreqSpin->setSuffix(u" GHz"_s);
    m_restFreqSpin->setValue(0.0);
    // Only relevant for FWHM (velocity-axis conversion)
    form->addRow(u"Rest frequency"_s, m_restFreqSpin);

    // Disable rest-freq when EW is chosen
    auto updateRestFreq = [this]() {
        m_restFreqSpin->setEnabled(m_methodCombo->currentData().toString() == u"fwhm"_s);
    };
    updateRestFreq();
    connect(m_methodCombo, &QComboBox::currentIndexChanged, this, updateRestFreq);

    // ── Channel range ─────────────────────────────────────────────────────────
    m_chanStartSpin = new QSpinBox(this);
    m_chanEndSpin   = new QSpinBox(this);
    m_chanStartSpin->setRange(1, m_depth);
    m_chanEndSpin->setRange(1, m_depth);
    m_chanStartSpin->setValue(1);
    m_chanEndSpin->setValue(m_depth);
    form->addRow(u"Start channel"_s, m_chanStartSpin);
    form->addRow(u"End channel"_s,   m_chanEndSpin);

    // ── Threshold mask ────────────────────────────────────────────────────────
    m_maskCheck = new QCheckBox(u"Enable threshold mask"_s, this);
    m_maskCheck->setChecked(false);
    form->addRow(u"Mask"_s, m_maskCheck);

    m_thresholdSpin = new QDoubleSpinBox(this);
    m_thresholdSpin->setRange(-1.0e30, 1.0e30);
    m_thresholdSpin->setDecimals(6);
    m_thresholdSpin->setValue(initialThreshold);
    m_thresholdSpin->setEnabled(false);
    form->addRow(u"Mask threshold"_s, m_thresholdSpin);

    connect(m_maskCheck, &QCheckBox::toggled, m_thresholdSpin, &QWidget::setEnabled);

    // ── Buttons ───────────────────────────────────────────────────────────────
    m_buttons = new QDialogButtonBox(this);
    auto *computeBtn = m_buttons->addButton(u"Compute"_s, QDialogButtonBox::AcceptRole);
    m_buttons->addButton(QDialogButtonBox::Cancel);
    mainLayout->addWidget(m_buttons);

    connect(computeBtn,   &QPushButton::clicked,       this, &LinewidthDialog::onCompute);
    connect(m_buttons,    &QDialogButtonBox::rejected,  this, &QDialog::reject);
}

// ─────────────────────────────────────────────────────────────────────────────

void LinewidthDialog::onCompute()
{
    const int chanStart = m_chanStartSpin->value() - 1;  // 0-based
    const int chanEnd   = m_chanEndSpin->value()   - 1;
    if (chanStart > chanEnd) {
        QMessageBox::warning(this, u"Line-Width Map"_s,
                             u"Start channel must be ≤ end channel."_s);
        return;
    }

    const bool   maskEnabled = m_maskCheck->isChecked();
    const float  threshold   = static_cast<float>(m_thresholdSpin->value());
    const QString method     = m_methodCombo->currentData().toString();
    const QString datasetId  = m_datasetId;

    setUiBusy(true);

    m_progress = new QProgressDialog(u"Computing line-width map…"_s,
                                     QString(), 0, 0, this);
    m_progress->setWindowModality(Qt::WindowModal);
    m_progress->setMinimumDuration(400);
    m_progress->show();

    BackendClient *client = m_client;
    client->setSessionId(m_sessionId);  // FIX 5: ensure session ID is set before request

    // Capture everything needed by the worker by value.
    auto *watcher = new QFutureWatcher<BackendLinewidthResult>(this);
    QPointer<LinewidthDialog> self(this);

    connect(watcher, &QFutureWatcher<BackendLinewidthResult>::finished,
            watcher, [self, watcher, method]() {
        const auto result = watcher->result();
        watcher->deleteLater();

        if (!self) {
            return;
        }

        if (self->m_progress) {
            self->m_progress->close();
            self->m_progress->deleteLater();
        }
        self->setUiBusy(false);

        if (!result.valid) {
            QMessageBox::critical(self, u"Line-Width Map"_s,
                                  result.error.isEmpty()
                                          ? u"Backend returned an error."_s
                                          : result.error);
            return;
        }

        // Parse the two binary frames from binaryPayload using
        // BackendClient::parseBinaryFrame.
        int offset = 0;
        BinaryArrayResult fwhmFrame, ewFrame;
        const bool ok1 = BackendClient::parseBinaryFrame(result.binaryPayload, offset, fwhmFrame);
        const bool ok2 = BackendClient::parseBinaryFrame(result.binaryPayload, offset, ewFrame);

        // Choose the frame the user requested.
        const bool wantFwhm = (method == u"fwhm"_s);

        const BinaryArrayResult &frame = wantFwhm ? fwhmFrame : ewFrame;
        const bool ok = wantFwhm ? ok1 : ok2;

        if (!ok || frame.data.isEmpty()) {
            QMessageBox::critical(self, u"Line-Width Map"_s,
                                  u"Failed to parse the result frame from the server."_s);
            return;
        }

        // Compute scalar range by reinterpreting raw bytes as float32.
        float rangeMin = 0.f, rangeMax = 1.f;
        const int nFloats = frame.data.size() / static_cast<int>(sizeof(float));
        if (nFloats > 0) {
            const float *ptr = reinterpret_cast<const float *>(frame.data.constData());
            rangeMin = *std::min_element(ptr, ptr + nFloats);
            rangeMax = *std::max_element(ptr, ptr + nFloats);
            if (rangeMin == rangeMax) {
                rangeMax = rangeMin + 1.f;
            }
        }

        if (result.width == 0 || result.height == 0) {
            QMessageBox::warning(self, u"Line-Width Map"_s,
                                 u"Result is empty — all pixels failed to fit.\n"
                                 "Try widening the channel range or lowering the threshold."_s);
            return;
        }

        const QString unit  = wantFwhm ? result.fwhmUnit : result.ewUnit;
        const QString label = wantFwhm
                ? u"FWHM"_s + (unit.isEmpty() ? QString() : u" ["_s + unit + u"]"_s)
                : u"EW"_s   + (unit.isEmpty() ? QString() : u" ["_s + unit + u"]"_s);

        auto *win = new ScienceMapWindow(label,
                                         frame.data,
                                         result.width,
                                         result.height,
                                         rangeMin,
                                         rangeMax,
                                         /* parent */ nullptr);
        win->show();

        // Close the dialog after successfully opening the result window.
        if (self) {
            self->accept();
        }
    });

    watcher->setFuture(QtConcurrent::run(
            [client, datasetId, chanStart, chanEnd, maskEnabled, threshold]() {
                return client->fetchLinewidthBinary(datasetId,
                                                    chanStart, chanEnd,
                                                    maskEnabled, threshold);
            }));
}

void LinewidthDialog::setUiBusy(bool busy)
{
    m_methodCombo->setEnabled(!busy);
    m_restFreqSpin->setEnabled(!busy && m_methodCombo->currentData().toString() == u"fwhm"_s);
    m_chanStartSpin->setEnabled(!busy);
    m_chanEndSpin->setEnabled(!busy);
    m_maskCheck->setEnabled(!busy);
    m_thresholdSpin->setEnabled(!busy && m_maskCheck->isChecked());
    m_buttons->setEnabled(!busy);
}
