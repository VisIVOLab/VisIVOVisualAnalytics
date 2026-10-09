#ifndef FitsHeaderCard_h
#define FitsHeaderCard_h

// FitsHeaderCard.h
// Split a raw 80-column FITS card into keyword, value and comment.
//
// Header-only and free of Qt widgets so the rule can be tested directly. It has
// to be, because the two obvious implementations are both wrong on real files:
// splitting on the first '/' cuts "RA---TAN / projection" in the wrong place
// when the value itself is a quoted string containing a slash (a BUNIT of
// 'Jy/beam', a DATE-OBS, a path in a HISTORY line), and splitting on the last
// one eats a comment that contains a slash of its own.
//
// The FITS rule is simpler than either: a value may be a quoted string, and a
// '/' inside quotes is part of it; the first '/' OUTSIDE quotes starts the
// comment. Doubled quotes ('') are an escaped quote within a string.

#include <QString>

namespace visivo {

struct FitsHeaderCard
{
    QString key;
    QString value;
    QString comment;
    /// True for cards that carry free text rather than a value: HISTORY,
    /// COMMENT, CONTINUE and the blank-keyword lines. They are shown but not
    /// offered for editing — there is no single value to change.
    bool commentary{ false };
};

/// Parse one card. Never throws; an unparsable line comes back as commentary,
/// which is how it should be displayed.
inline FitsHeaderCard parseFitsHeaderCard(const QString &raw)
{
    FitsHeaderCard card;
    const QString line = raw;
    const QString keyField = line.left(8).trimmed().toUpper();

    if (keyField.isEmpty() || keyField == QStringLiteral("HISTORY")
        || keyField == QStringLiteral("COMMENT") || keyField == QStringLiteral("CONTINUE")
        || keyField == QStringLiteral("END")) {
        card.key = keyField;
        card.value = line.mid(8).trimmed();
        card.commentary = true;
        return card;
    }
    // A value card is "KEYWORD = value [/ comment]". Without the '=' there is
    // no value to speak of, whatever the keyword looks like.
    const int equals = line.indexOf(u'=');
    if (equals < 0 || equals > 9) {
        card.key = keyField;
        card.value = line.mid(8).trimmed();
        card.commentary = true;
        return card;
    }

    card.key = keyField;
    const QString rest = line.mid(equals + 1);

    bool inString = false;
    int commentAt = -1;
    for (int i = 0; i < rest.size(); ++i) {
        const QChar c = rest.at(i);
        if (c == u'\'') {
            // '' inside a string is one escaped quote, not the end of it.
            if (inString && i + 1 < rest.size() && rest.at(i + 1) == u'\'') {
                ++i;
                continue;
            }
            inString = !inString;
            continue;
        }
        if (c == u'/' && !inString) {
            commentAt = i;
            break;
        }
    }

    QString value = commentAt >= 0 ? rest.left(commentAt) : rest;
    card.comment = commentAt >= 0 ? rest.mid(commentAt + 1).trimmed() : QString();
    value = value.trimmed();
    // Keep the quotes off the displayed value, but remember they were there by
    // unescaping the doubled ones; the editor re-quotes on the way out only
    // when the user's text would otherwise read as a number.
    if (value.size() >= 2 && value.startsWith(u'\'') && value.endsWith(u'\'')) {
        value = value.mid(1, value.size() - 2).replace(QStringLiteral("''"),
                                                       QStringLiteral("'"));
        value = value.trimmed();
    }
    card.value = value;
    return card;
}

/// Whether ``text`` would be read back as a number or a boolean rather than as
/// the string it is meant to be — in which case the editor sends it quoted.
inline bool needsQuoting(const QString &text)
{
    if (text.isEmpty())
        return false;
    bool ok = false;
    text.toDouble(&ok);
    if (ok)
        return true;
    const QString lowered = text.trimmed().toLower();
    return lowered == QStringLiteral("t") || lowered == QStringLiteral("f")
            || lowered == QStringLiteral("true") || lowered == QStringLiteral("false");
}

}   // namespace visivo

#endif   // FitsHeaderCard_h
