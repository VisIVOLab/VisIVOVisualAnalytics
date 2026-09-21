#ifndef FitsHeaderEditorDialog_h
#define FitsHeaderEditorDialog_h

// FitsHeaderEditorDialog.h
// Edit a FITS header into a corrected copy.
//
// Ported from the legacy client's FitsHeaderModifierDialog (ViaLactea Visual
// Analytics 1.7.4), which appeared only when a file failed to open, offered
// three keyword groups — CTYPE, CUNIT, CDELT — and reported "File has been
// saved!" whether or not the values typed produced a header anything could
// read. Here the whole header is in front of the user, any keyword can be
// changed, and the backend answers the question the legacy left open: after the
// edit, does this file have a WCS?
//
// The parsing of a raw 80-column card into keyword / value / comment lives in
// FitsHeaderCard.h, where it can be tested against the cases that break it
// (quoted strings containing a slash, CONTINUE, HISTORY).

#include "BackendClient.h"

#include <QDialog>
#include <QFutureWatcher>
#include <QPointer>
#include <QString>
#include <QStringList>

class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QTableWidget;

class FitsHeaderEditorDialog : public QDialog
{
    Q_OBJECT

public:
    /// @param path  the FITS whose header this is, on the BACKEND's filesystem
    /// @param cards the raw 80-column cards, as /v1/files/header returns them
    FitsHeaderEditorDialog(const QString &path, const QStringList &cards,
                           const QString &backendUrl, const QString &backendToken,
                           QWidget *parent = nullptr);
    ~FitsHeaderEditorDialog() override;

signals:
    /// The corrected copy, in the workspace, ready to open.
    void saved(const QString &backendPath);

private slots:
    void addRow();
    void removeSelectedRow();
    void onFinished();

private:
    void buildUi();
    void populate(const QStringList &cards);
    void accept() override;

    struct OriginalCard
    {
        QString key;
        QString value;
        QString comment;
    };

    QString m_path;
    QString m_backendUrl;
    QString m_backendToken;
    QList<OriginalCard> m_original;

    QFutureWatcher<BackendHeaderPatchResult> m_watcher;

    QPointer<QTableWidget> m_table;
    QPointer<QLineEdit> m_outputName;
    QPointer<QLabel> m_status;
    QPointer<QDialogButtonBox> m_buttons;
};

#endif   // FitsHeaderEditorDialog_h
