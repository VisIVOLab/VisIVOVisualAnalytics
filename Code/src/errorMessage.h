#ifndef ERRORMESSAGE_H
#define ERRORMESSAGE_H

#include <QMessageBox>

/**
 * @brief throwError
 * A standardised error popup message that can be thrown from anywhere.
 * @param text The error message
 * @param info More detailed information, such as the next step to take or
 *             steps that could resolve the issue.
 */
static void throwError(const QString &text, const QString &info)
{
    QMessageBox e;
    e.setIcon(QMessageBox::Critical);
    e.setWindowTitle("Error!");
    e.setText(text);
    e.setInformativeText(info);
    e.setStandardButtons(QMessageBox::Ok);
    e.exec();
}

/**
 * @brief throwMessage
 * A standardised information popup message that can be thrown from anywhere.
 * @param text The basic information
 * @param info More detailed information
 */
static void throwMessage(const QString &text, const QString &info)
{
    QMessageBox e;
    e.setIcon(QMessageBox::Information);
    e.setWindowTitle("Information:");
    e.setText(text);
    e.setInformativeText(info);
    e.setStandardButtons(QMessageBox::Ok);
    e.exec();
}

/**
 * @brief throwWarning
 * A standardised warning popup message that can be thrown from anywhere.
 * @param text The basic information
 * @param info More detailed information
 */
static void throwWarning(const QString &text, const QString &info)
{
    QMessageBox e;
    e.setIcon(QMessageBox::Warning);
    e.setWindowTitle("Warning!");
    e.setText(text);
    e.setInformativeText(info);
    e.setStandardButtons(QMessageBox::Ok);
    e.exec();
}

#endif // ERRORMESSAGE_H
