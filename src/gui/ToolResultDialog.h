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
    void addSection(const QString &sectionTitle,
                    const QList<QPair<QString, QString>> &rows);

private:
    QLabel      *m_titleLabel    { nullptr };
    QLabel      *m_subtitleLabel { nullptr };
    QVBoxLayout *m_bodyLayout    { nullptr };
};
