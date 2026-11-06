#ifndef COSMOLOGYTOOLWIDGETS_H
#define COSMOLOGYTOOLWIDGETS_H
// CosmologyToolWidgets.h
// The widgets the angular power spectrum and the skeleton / void detector
// share between the image viewer and the cube viewer (issue #29):
//
//   * AngularPowerSettings, SkeletonSettings — each tool's controls, shown in
//     Inspector ▸ Parameters through the viewer's tool-settings stack (never a
//     dock of their own);
//   * PowerSpectrumWindow — the result: P(ℓ) on log axes, the fitted slope and
//     its range, CSV and PNG export.

#include "BackendClient.h"
#include "PowerSpectrumSlope.h"

#include <QPointer>
#include <QWidget>

#include <functional>

class QCheckBox;
class QComboBox;
class QCPGraph;
class QCustomPlot;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;

class AngularPowerSettings : public QWidget
{
    Q_OBJECT
public:
    explicit AngularPowerSettings(QWidget *parent = nullptr);
    int nBins() const;
    /// What the tool will analyse ("this image", "Moment 0 (channels 12–80)").
    void setSourceText(const QString &text);
    /// Greys Compute out with the reason when the source is not there.
    void setAvailable(bool available, const QString &reason = {});
    void setBusy(bool busy);

signals:
    void runRequested();

private:
    QPointer<QLabel> m_source;
    QPointer<QSpinBox> m_bins;
    QPointer<QPushButton> m_run;
    QPointer<QLabel> m_hint;
    bool m_available{ true };
    bool m_busy{ false };
    void refreshRun();
};

class SkeletonSettings : public QWidget
{
    Q_OBJECT
public:
    explicit SkeletonSettings(QWidget *parent = nullptr);
    BackendSkeletonRequest request() const;
    void setSourceText(const QString &text);
    void setAvailable(bool available, const QString &reason = {});
    void setBusy(bool busy);
    /// The last result's one-line summary, under the buttons.
    void setResultText(const QString &text);
    /// Hide Clear where the result is not an overlay (the cube's mask is a map).
    void setClearVisible(bool visible);

signals:
    void runRequested();
    void clearRequested();

private:
    QPointer<QLabel> m_source;
    QPointer<QComboBox> m_mode;
    QPointer<QCheckBox> m_thresholdAuto;
    QPointer<QLineEdit> m_threshold;
    QPointer<QDoubleSpinBox> m_smooth;
    QPointer<QCheckBox> m_prune;
    QPointer<QLineEdit> m_pruneValue;
    QPointer<QPushButton> m_run;
    QPointer<QPushButton> m_clear;
    QPointer<QLabel> m_hint;
    QPointer<QLabel> m_result;
    bool m_available{ true };
    bool m_busy{ false };
    void refreshRun();
};

class PowerSpectrumWindow : public QWidget
{
    Q_OBJECT
public:
    /// @p onFit is told every fit the user settles on, so the product's record
    /// carries the range the slope was measured over.
    PowerSpectrumWindow(const BackendAngularPowerResult &spectrum, const QString &sourceLabel,
                        const visivo::PowerSlopeOptions &options, QWidget *parent = nullptr);

    void setFitCallback(std::function<void(const visivo::PowerSlopeFit &,
                                           const visivo::PowerSlopeOptions &)> onFit);
    visivo::PowerSlopeOptions options() const;
    visivo::PowerSlopeFit fit() const { return m_fit; }

    /// One line for a status bar: "slope −2.31 ± 0.05 over ℓ 120–4500 (23 bins)".
    static QString slopeSummary(const visivo::PowerSlopeFit &fit, bool ellIsAngular);

private:
    void refit();
    void redraw();
    void exportCsv();
    void savePng();

    BackendAngularPowerResult m_spectrum;
    QString m_sourceLabel;
    visivo::PowerSlopeFit m_fit;
    std::function<void(const visivo::PowerSlopeFit &, const visivo::PowerSlopeOptions &)> m_onFit;

    QPointer<QCustomPlot> m_plot;
    QPointer<QDoubleSpinBox> m_ellMin;
    QPointer<QDoubleSpinBox> m_ellMax;
    QPointer<QSpinBox> m_minModes;
    QPointer<QLabel> m_slope;
};

#endif   // COSMOLOGYTOOLWIDGETS_H
