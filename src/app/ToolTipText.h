#pragma once

// Tooltip text for long values with no spaces (paths, session ids, URLs).
//
// Qt never wraps a plain-text tooltip, so the end of a long path was cut at the
// screen's edge. Rich text with zero-width break points wrapped, but Qt's
// tooltip label measured it short: the last line was cut in half and short
// values lost their last character. Plain text with explicit line breaks is
// measured exactly, so a long value is broken into lines here, preferably just
// after a separator, and anything that fits on one line is returned untouched.

#include <QString>
#include <QStringList>

namespace visivo {

inline QString wrappedToolTip(const QString &text, int maxLine = 64)
{
    if (maxLine < 8 || text.size() <= maxLine || text.contains(u'\n'))
        return text;
    const auto isBreakAfter = [](QChar c) {
        return c == u'/' || c == u'\\' || c == u'_' || c == u'-' || c == u'.' || c == u' '
               || c == u',' || c == u':' || c == u'?' || c == u'&' || c == u'=';
    };
    QStringList lines;
    qsizetype start = 0;
    while (text.size() - start > maxLine) {
        // The last separator that keeps the line within maxLine, but not one
        // so early that the line would be mostly empty; else a hard break.
        qsizetype cut = -1;
        for (qsizetype i = start + maxLine - 1; i >= start + maxLine / 2; --i) {
            if (isBreakAfter(text.at(i))) {
                cut = i + 1;
                break;
            }
        }
        if (cut < 0)
            cut = start + maxLine;
        lines.append(text.mid(start, cut - start));
        start = cut;
    }
    lines.append(text.mid(start));
    return lines.join(u'\n');
}

}  // namespace visivo
