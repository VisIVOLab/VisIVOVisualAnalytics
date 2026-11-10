#ifndef VBTBYTEORDERPROMPT_H
#define VBTBYTEORDERPROMPT_H

// The question asked when a VBT's values look like the other byte order than
// its header declares (issue #165). The wording, the default and the memory
// are in app/VbtByteOrder.h; this is only the box.

#include "BusyStatus.h"
#include "app/VbtByteOrder.h"

#include <QElapsedTimer>
#include <QFileInfo>
#include <QMessageBox>
#include <QPushButton>

namespace visivo {

/// Ask which byte order to read the VBT @p path in. A button's answer is
/// remembered for @p backendUrl (the opens that follow send it); Escape or
/// closing the box keeps the order @p readIn the table was just opened in, and
/// remembers nothing. Returns true when the answer is not @p readIn: the
/// caller opens it again. Without a warning, nothing is asked. @p askedMs,
/// when given, gets the time the box was up added to it: the user's time, not
/// the open's (the Data Hub's "Last 5 jobs", as the viewer chooser does).
inline bool askVbtByteOrder(QWidget *parent, const QString &backendUrl, const QString &path,
                            const VbtByteOrderWarning &warning, const QString &readIn,
                            qint64 *askedMs = nullptr)
{
    if (!warning.present)
        return false;
    const QString name = QFileInfo(vbtDataPath(path)).fileName();
    QMessageBox box(parent);
    box.setWindowTitle(QStringLiteral("Byte order of %1").arg(name));   // not shown on macOS
    box.setIcon(QMessageBox::Warning);
    box.setTextFormat(Qt::RichText);   // escaped: a file name is never markup
    box.setText(vbtByteOrderBoxText(warning, name));
    box.setInformativeText(vbtByteOrderEvidenceHtml(warning));
    QPushButton *asSuggested = box.addButton(
            QStringLiteral("Open as %1-endian").arg(warning.suggested), QMessageBox::AcceptRole);
    QPushButton *asDeclared =
            box.addButton(QStringLiteral("Open as declared"), QMessageBox::RejectRole);
    // The suggested order is the default only on overwhelming evidence;
    // otherwise the header, the file's own statement, is. Escape keeps it too.
    box.setDefaultButton(vbtByteOrderDefaultsToSuggested(warning, readIn) ? asSuggested
                                                                          : asDeclared);
    QPushButton *keep = box.addButton(QMessageBox::Cancel);
    keep->hide();   // Escape / the close box: no answer, the order read now stays
    box.setEscapeButton(keep);
    {
        const BusyPause asking;   // the question is what is read now
        QElapsedTimer up;
        up.start();
        box.exec();
        if (askedMs)
            *askedMs += up.elapsed();
    }
    QString order;
    if (box.clickedButton() == asSuggested)
        order = warning.suggested;
    else if (box.clickedButton() == asDeclared)
        order = warning.declared;
    else
        return false;
    // The declared order is remembered too: asked for explicitly, the opens
    // that follow on their own are not warned again.
    rememberVbtByteOrder(backendUrl, path, order);
    return order != readIn;
}

} // namespace visivo

#endif // VBTBYTEORDERPROMPT_H
