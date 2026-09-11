#pragma once

#include <QDialog>
#include <QList>
#include <QPair>
#include <QString>

class QLabel;
class QVBoxLayout;

/// Generic result-display dialog for analytical tools.
///
/// Designed to show structured output (title, optional subtitle, multiple
/// key-value sections) in a clean, dark-themed layout consistent with the
/// VisIVO cube viewer sidebar style.
///
/// Usage (non-modal — caller must allocate on the heap):
///   auto *dlg = new ToolResultDialog(this);
///   dlg->setTitle("Noise Estimation");
///   dlg->setSubtitle("Channels 0–127  ·  region x=0..511, y=0..511");
///   dlg->addSection("Sigma", {{"Mean", "0.00123"}, {"Median", "0.00119"}, ...});
///   dlg->addSection("MAD",   {{"Mean", "0.00118"}, ...});
///   dlg->show();  // non-blocking; WA_DeleteOnClose handles cleanup
class ToolResultDialog : public QDialog
{
    Q_OBJECT
public:
    explicit ToolResultDialog(QWidget *parent = nullptr);

    /// Set the primary title shown in the header and window title bar.
    void setTitle(const QString &title);

    /// Set an optional subtitle / context line shown below the title.
    /// Pass an empty string to hide the subtitle area.
    void setSubtitle(const QString &subtitle);

    /// Append a labeled section of key-value rows to the body.
    /// Sections are stacked vertically in the order they are added.
    /// Build the whole window from a RECORD: the exact recipe of a result
    /// window, as a plain QVariantMap that can be stored on a product and
    /// replayed later.
    ///
    /// This exists so that reopening a result from its Session Data row gives
    /// back the SAME window, not a reconstruction that resembles it. Deriving
    /// the layout from the product's provenance parameters instead produced a
    /// visibly different window — one flat list where there had been a Sigma
    /// and a MAD section, keys reading "σ mean" instead of "Mean", and the
    /// product's label in place of the tool's title (user report). One record,
    /// one code path, no second opinion.
    ///
    /// Shape:
    /// @code
    /// { "title": "Noise Estimation",
    ///   "subtitle": "Channels 0..397 …",
    ///   "sections": [ { "title": "Sigma",
    ///                   "rows": [ ["Mean", "0.0034779168"], … ] }, … ] }
    /// @endcode
    void applyRecord(const QVariantMap &record);

    /// Assemble a record. Sections are given in display order; each row is a
    /// label/value pair, also in display order (a QVariantMap would sort them).
    static QVariantMap makeRecord(
            const QString &title, const QString &subtitle,
            const QList<QPair<QString, QList<QPair<QString, QString>>>> &sections);

    void addSection(const QString &sectionTitle,
                    const QList<QPair<QString, QString>> &rows);

protected:
    /// On first show, anchor the dialog to the right edge of the parent
    /// window so it reads as a side-sheet rather than a free-floating popup.
    void showEvent(QShowEvent *event) override;

private:
    QLabel      *m_titleLabel    { nullptr };
    QLabel      *m_subtitleLabel { nullptr };
    QVBoxLayout *m_bodyLayout    { nullptr };
    bool         m_anchored      { false };
};
