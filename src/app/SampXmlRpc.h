// SampXmlRpc.h – XML-RPC encoding and decoding for the SAMP Standard Profile
//
// SAMP speaks XML-RPC in both directions: VisIVO calls the hub
// (samp.hub.register, notify, reply, …), and the hub calls VisIVO back on its
// callback server (samp.client.receiveNotification, receiveCall,
// receiveResponse). Both ends need the same codec, so it lives here, once,
// header-only and on QtCore alone (QXmlStreamReader/Writer), which lets the
// unit tests exercise it without QtXml, QtWidgets or a hub.
//
// SAMP itself only ever sends strings, lists and maps (SAMP 1.3 §3.3), but a
// hub is an ordinary XML-RPC server and may answer with the other scalar
// types, so all of them are decoded.

#ifndef SAMPXMLRPC_H
#define SAMPXMLRPC_H

#include <QByteArray>
#include <QString>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>

#include <limits>

namespace SampXmlRpc {

/// A decoded <methodCall>.
struct Call
{
    QString method;
    QVariantList params;
    QString error;   ///< non-empty when the body is not a well-formed call
    bool isValid() const { return error.isEmpty() && !method.isEmpty(); }
};

namespace detail {

inline void writeValue(QXmlStreamWriter &xml, const QVariant &v)
{
    xml.writeStartElement(QStringLiteral("value"));
    switch (v.typeId()) {
    case QMetaType::Bool:
        xml.writeTextElement(QStringLiteral("boolean"),
                             v.toBool() ? QStringLiteral("1") : QStringLiteral("0"));
        break;
    case QMetaType::Int:
    case QMetaType::UInt:
    case QMetaType::Long:
    case QMetaType::LongLong:
    case QMetaType::ULongLong: {
        // XML-RPC integers are 32-bit (<i4>); a wider one goes as a string,
        // which is what SAMP wants for every scalar anyway.
        bool ok = false;
        const qlonglong n = v.toLongLong(&ok);
        if (ok && n >= std::numeric_limits<qint32>::min()
            && n <= std::numeric_limits<qint32>::max())
            xml.writeTextElement(QStringLiteral("i4"), QString::number(n));
        else
            xml.writeTextElement(QStringLiteral("string"), v.toString());
        break;
    }
    case QMetaType::Double:
    case QMetaType::Float:
        xml.writeTextElement(QStringLiteral("double"), QString::number(v.toDouble(), 'g', 17));
        break;
    case QMetaType::QByteArray:
        xml.writeTextElement(QStringLiteral("base64"),
                             QString::fromLatin1(v.toByteArray().toBase64()));
        break;
    case QMetaType::QVariantMap: {
        xml.writeStartElement(QStringLiteral("struct"));
        const QVariantMap m = v.toMap();
        for (auto it = m.constBegin(); it != m.constEnd(); ++it) {
            xml.writeStartElement(QStringLiteral("member"));
            xml.writeTextElement(QStringLiteral("name"), it.key());
            writeValue(xml, it.value());
            xml.writeEndElement();
        }
        xml.writeEndElement();
        break;
    }
    case QMetaType::QVariantList:
    case QMetaType::QStringList: {
        xml.writeStartElement(QStringLiteral("array"));
        xml.writeStartElement(QStringLiteral("data"));
        for (const QVariant &item : v.toList())
            writeValue(xml, item);
        xml.writeEndElement();
        xml.writeEndElement();
        break;
    }
    default:
        // Strings, and anything else as its string form. A null QVariant
        // becomes the empty string: SAMP has no nil, and an empty string is
        // what a void SAMP method returns.
        xml.writeTextElement(QStringLiteral("string"), v.toString());
        break;
    }
    xml.writeEndElement();
}

/// Deeper nesting is refused: SAMP needs three levels (message → params →
/// row-list), and the callback server decodes before it can check the
/// private key, so unbounded recursion would let any local process exhaust
/// the stack.
inline constexpr int kMaxDepth = 32;

inline QVariant readValue(QXmlStreamReader &xml, int depth = 0);

/// At <struct>; returns at </struct>.
inline QVariant readStruct(QXmlStreamReader &xml, int depth)
{
    QVariantMap map;
    while (xml.readNextStartElement()) {
        if (xml.name() != u"member") {
            xml.raiseError(QStringLiteral("expected <member> in <struct>"));
            return {};
        }
        QString name;
        QVariant value;
        bool haveName = false;
        bool haveValue = false;
        while (xml.readNextStartElement()) {
            if (xml.name() == u"name") {
                name = xml.readElementText();
                haveName = true;
            } else if (xml.name() == u"value") {
                value = readValue(xml, depth + 1);
                haveValue = true;
            } else {
                xml.raiseError(QStringLiteral("unexpected <%1> in <member>")
                                       .arg(xml.name().toString()));
                return {};
            }
        }
        if (xml.hasError())
            return {};
        if (!haveName || !haveValue) {
            xml.raiseError(QStringLiteral("<member> without a name or a value"));
            return {};
        }
        map.insert(name, value);
    }
    return map;
}

/// At <array>; returns at </array>.
inline QVariant readArray(QXmlStreamReader &xml, int depth)
{
    QVariantList list;
    while (xml.readNextStartElement()) {
        if (xml.name() != u"data") {
            xml.raiseError(QStringLiteral("expected <data> in <array>"));
            return {};
        }
        while (xml.readNextStartElement()) {
            if (xml.name() != u"value") {
                xml.raiseError(QStringLiteral("expected <value> in <data>"));
                return {};
            }
            list.append(readValue(xml, depth + 1));
            if (xml.hasError())
                return {};
        }
    }
    return list;
}

/// At <value>; returns at </value>. A <value> with no type element is an
/// implicit string (XML-RPC spec).
inline QVariant readValue(QXmlStreamReader &xml, int depth)
{
    if (depth > kMaxDepth) {
        xml.raiseError(QStringLiteral("XML-RPC value nested too deeply"));
        return {};
    }
    QString implicitText;
    while (!xml.atEnd()) {
        const auto token = xml.readNext();
        if (token == QXmlStreamReader::Characters) {
            implicitText += xml.text();
        } else if (token == QXmlStreamReader::EndElement) {
            return implicitText;
        } else if (token == QXmlStreamReader::StartElement) {
            const QStringView type = xml.name();
            QVariant v;
            if (type == u"string") {
                v = xml.readElementText();
            } else if (type == u"i4" || type == u"int" || type == u"i8") {
                bool ok = false;
                const qlonglong n = xml.readElementText().trimmed().toLongLong(&ok);
                if (!ok) {
                    xml.raiseError(QStringLiteral("malformed integer"));
                    return {};
                }
                v = n;
            } else if (type == u"boolean") {
                v = xml.readElementText().trimmed() == u"1";
            } else if (type == u"double") {
                bool ok = false;
                const double d = xml.readElementText().trimmed().toDouble(&ok);
                if (!ok) {
                    xml.raiseError(QStringLiteral("malformed double"));
                    return {};
                }
                v = d;
            } else if (type == u"base64") {
                v = QByteArray::fromBase64(xml.readElementText().trimmed().toLatin1());
            } else if (type == u"dateTime.iso8601") {
                v = xml.readElementText();
            } else if (type == u"nil") {
                xml.skipCurrentElement();
            } else if (type == u"struct") {
                v = readStruct(xml, depth);
            } else if (type == u"array") {
                v = readArray(xml, depth);
            } else {
                xml.raiseError(QStringLiteral("unknown XML-RPC type <%1>").arg(type.toString()));
                return {};
            }
            if (xml.hasError())
                return {};
            // Only whitespace may follow the typed element inside <value>.
            while (!xml.atEnd()) {
                const auto next = xml.readNext();
                if (next == QXmlStreamReader::EndElement)
                    return v;
                if (next == QXmlStreamReader::StartElement) {
                    xml.raiseError(QStringLiteral("more than one element in <value>"));
                    return {};
                }
            }
            return v;
        }
    }
    return {};
}

/// At <params>; reads every <param><value>.
inline QVariantList readParams(QXmlStreamReader &xml)
{
    QVariantList params;
    while (xml.readNextStartElement()) {
        if (xml.name() != u"param") {
            xml.raiseError(QStringLiteral("expected <param> in <params>"));
            return {};
        }
        bool haveValue = false;
        while (xml.readNextStartElement()) {
            if (xml.name() != u"value" || haveValue) {
                xml.raiseError(QStringLiteral("a <param> holds exactly one <value>"));
                return {};
            }
            params.append(readValue(xml));
            haveValue = true;
            if (xml.hasError())
                return {};
        }
        if (!haveValue) {
            xml.raiseError(QStringLiteral("<param> without a <value>"));
            return {};
        }
    }
    return params;
}

/// Move to the root element. XML-RPC has no use for a DTD, and a DTD is how
/// a document declares entities: refusing it means no entity can expand a
/// small request into a large one before the private key is checked.
inline bool readRoot(QXmlStreamReader &xml, QStringView expected, QString &error)
{
    while (!xml.atEnd()) {
        const auto token = xml.readNext();
        if (token == QXmlStreamReader::DTD) {
            error = QStringLiteral("a DTD is not allowed in XML-RPC");
            return false;
        }
        if (token == QXmlStreamReader::StartElement) {
            if (xml.name() == expected)
                return true;
            error = QStringLiteral("not an XML-RPC %1").arg(expected.toString());
            return false;
        }
    }
    error = xml.hasError() ? xml.errorString()
                           : QStringLiteral("not an XML-RPC %1").arg(expected.toString());
    return false;
}

}  // namespace detail

/// A <methodCall> body.
inline QByteArray methodCall(const QString &method, const QVariantList &params)
{
    QByteArray out;
    QXmlStreamWriter xml(&out);
    xml.writeStartDocument();
    xml.writeStartElement(QStringLiteral("methodCall"));
    xml.writeTextElement(QStringLiteral("methodName"), method);
    xml.writeStartElement(QStringLiteral("params"));
    for (const QVariant &p : params) {
        xml.writeStartElement(QStringLiteral("param"));
        detail::writeValue(xml, p);
        xml.writeEndElement();
    }
    xml.writeEndElement();
    xml.writeEndElement();
    xml.writeEndDocument();
    return out;
}

/// A successful <methodResponse> carrying @p value.
inline QByteArray methodResponse(const QVariant &value)
{
    QByteArray out;
    QXmlStreamWriter xml(&out);
    xml.writeStartDocument();
    xml.writeStartElement(QStringLiteral("methodResponse"));
    xml.writeStartElement(QStringLiteral("params"));
    xml.writeStartElement(QStringLiteral("param"));
    detail::writeValue(xml, value);
    xml.writeEndElement();
    xml.writeEndElement();
    xml.writeEndElement();
    xml.writeEndDocument();
    return out;
}

/// A <fault> response.
inline QByteArray faultResponse(int code, const QString &message)
{
    QByteArray out;
    QXmlStreamWriter xml(&out);
    xml.writeStartDocument();
    xml.writeStartElement(QStringLiteral("methodResponse"));
    xml.writeStartElement(QStringLiteral("fault"));
    QVariantMap fault;
    fault.insert(QStringLiteral("faultCode"), code);
    fault.insert(QStringLiteral("faultString"), message);
    detail::writeValue(xml, fault);
    xml.writeEndElement();
    xml.writeEndElement();
    xml.writeEndDocument();
    return out;
}

/// Decode a <methodCall> (what the hub POSTs to the callback server).
inline Call parseMethodCall(const QByteArray &body)
{
    Call call;
    QXmlStreamReader xml(body);
    if (!detail::readRoot(xml, u"methodCall", call.error))
        return call;
    while (xml.readNextStartElement()) {
        if (xml.name() == u"methodName")
            call.method = xml.readElementText().trimmed();
        else if (xml.name() == u"params")
            call.params = detail::readParams(xml);
        else
            xml.skipCurrentElement();
    }
    if (xml.hasError())
        call.error = xml.errorString();
    else if (call.method.isEmpty())
        call.error = QStringLiteral("methodCall without a methodName");
    return call;
}

/// Decode a <methodResponse>. On a fault or a malformed body, returns an
/// invalid QVariant and sets @p errorOut; a successful void call returns
/// whatever the server put in (usually the empty string).
inline QVariant parseMethodResponse(const QByteArray &body, QString &errorOut)
{
    QXmlStreamReader xml(body);
    QString rootError;
    if (!detail::readRoot(xml, u"methodResponse", rootError)) {
        errorOut = QStringLiteral("XML-RPC parse error: ") + rootError;
        return {};
    }
    QVariant result;
    bool haveResult = false;
    while (xml.readNextStartElement()) {
        if (xml.name() == u"params") {
            const QVariantList params = detail::readParams(xml);
            if (!params.isEmpty()) {
                result = params.first();
                haveResult = true;
            }
        } else if (xml.name() == u"fault") {
            QVariant fault;
            while (xml.readNextStartElement()) {
                if (xml.name() == u"value")
                    fault = detail::readValue(xml);
                else
                    xml.skipCurrentElement();
            }
            if (!xml.hasError()) {
                const QVariantMap fm = fault.toMap();
                errorOut = fm.value(QStringLiteral("faultString"),
                                    QStringLiteral("Unknown fault")).toString();
                return {};
            }
        } else {
            xml.skipCurrentElement();
        }
    }
    if (xml.hasError()) {
        errorOut = QStringLiteral("XML-RPC parse error: ") + xml.errorString();
        return {};
    }
    if (!haveResult) {
        errorOut = QStringLiteral("Empty XML-RPC response.");
        return {};
    }
    return result;
}

}  // namespace SampXmlRpc

#endif  // SAMPXMLRPC_H
