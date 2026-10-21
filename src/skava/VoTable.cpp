#include "VoTable.h"

#include <QXmlStreamReader>

using namespace Qt::StringLiterals;

QString VoTable::cell(int row, const QString &name) const
{
    const int col = fields.indexOf(name);
    if (row < 0 || row >= rows.size() || col < 0 || col >= rows[row].size())
        return {};
    return rows[row][col];
}

const VoTableService *VoTable::service(const QString &id) const
{
    for (const auto &s : services) {
        if (s.id == id)
            return &s;
    }
    return nullptr;
}

VoTable VoTable::parse(const QByteArray &xml)
{
    VoTable t;
    QXmlStreamReader r(xml);
    bool inResultsTable = false;   // inside the first TABLE of the document
    bool tableDone = false;
    bool sawVotable = false;
    QStringList row;
    VoTableService *svc = nullptr;   // the adhoc:service RESOURCE being read
    int resourceDepth = 0;
    QStringList resourceTypes;       // enclosing RESOURCE types, innermost last
    int svcDepth = -1;
    bool inInputParams = false;
    QString queryStatus, statusText;

    while (!r.atEnd()) {
        r.readNext();
        if (r.isStartElement()) {
            const auto name = r.name();
            const auto attrs = r.attributes();
            if (name == "VOTABLE"_L1) {
                sawVotable = true;
            } else if (name == "RESOURCE"_L1) {
                ++resourceDepth;
                // VOTable: type defaults to "results"; "meta" resources hold descriptions.
                resourceTypes << (attrs.hasAttribute("type"_L1) ? attrs.value("type"_L1).toString()
                                                                : u"results"_s);
                if (attrs.value("utype"_L1) == "adhoc:service"_L1) {
                    t.services.append({ attrs.value("ID"_L1).toString(), {}, {}, {} });
                    svc = &t.services.last();
                    svcDepth = resourceDepth;
                }
            } else if (name == "TABLE"_L1) {
                if (!tableDone && !svc
                    && (resourceTypes.isEmpty() || resourceTypes.last() != "meta"_L1))
                    inResultsTable = true;
            } else if (name == "FIELD"_L1 && inResultsTable) {
                t.fields << attrs.value("name"_L1).toString();
            } else if (name == "PARAM"_L1) {
                const QString pname = attrs.value("name"_L1).toString();
                const QString value = attrs.value("value"_L1).toString();
                if (svc) {
                    if (inInputParams)
                        svc->inputParams << pname;
                    else if (pname == "standardID"_L1)
                        svc->standardId = value;
                    else if (pname == "accessURL"_L1)
                        svc->accessUrl = value;
                } else if (inResultsTable) {
                    t.params.insert(pname, value);
                }
            } else if (name == "GROUP"_L1 && svc && attrs.value("name"_L1) == "inputParams"_L1) {
                inInputParams = true;
            } else if (name == "INFO"_L1 && attrs.value("name"_L1) == "QUERY_STATUS"_L1) {
                queryStatus = attrs.value("value"_L1).toString();
                statusText = r.readElementText(QXmlStreamReader::IncludeChildElements).trimmed();
            } else if ((name == "BINARY"_L1 || name == "BINARY2"_L1 || name == "FITS"_L1) && inResultsTable) {
                t.binary = true;
            } else if (name == "TR"_L1 && inResultsTable) {
                row.clear();
            } else if (name == "TD"_L1 && inResultsTable) {
                row << r.readElementText(QXmlStreamReader::IncludeChildElements);
            }
        } else if (r.isEndElement()) {
            const auto name = r.name();
            if (name == "TR"_L1 && inResultsTable) {
                t.rows << row;
            } else if (name == "TABLE"_L1 && inResultsTable) {
                inResultsTable = false;
                tableDone = true;
            } else if (name == "GROUP"_L1) {
                inInputParams = false;
            } else if (name == "RESOURCE"_L1) {
                if (svc && resourceDepth == svcDepth) {
                    svc = nullptr;
                    svcDepth = -1;
                }
                --resourceDepth;
                if (!resourceTypes.isEmpty())
                    resourceTypes.removeLast();
            }
        }
    }
    if (r.hasError()) {
        t.error = u"Malformed VOTable: %1"_s.arg(r.errorString());
        return t;
    }
    if (!sawVotable) {
        t.error = u"The response is not a VOTable."_s;
        return t;
    }
    if (queryStatus.compare("ERROR"_L1, Qt::CaseInsensitive) == 0) {
        t.error = statusText.isEmpty() ? u"The service reported an error."_s : statusText;
        return t;
    }
    if (t.binary) {
        t.error = u"The service answered with a binary VOTable; VisIVO reads TABLEDATA "
                  u"(RESPONSEFORMAT=votable/td)."_s;
        return t;
    }
    t.valid = true;
    return t;
}
