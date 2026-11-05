"""DataLink {links} for the node's SODA: what a client may cut from a dataset.

A DataLink response is a VOTable with one row per link and, for each link
to a service, a *service descriptor* (a ``RESOURCE`` of type ``meta``,
``utype="adhoc:service"``) listing the service's input parameters. Only the
node can write a useful descriptor for its SODA, because only the node can
read the file: the BAND interval, the observation time and the polarization
states (POL ``VALUES``) are taken from the dataset's own header, so a client
such as pyvo or TOPCAT offers the user the values the dataset actually has.

The module is transport-free: the router resolves IDs to paths and passes
the URLs; this module reads headers (``soda_cutout.describe``) and writes XML.
"""
from __future__ import annotations

from dataclasses import dataclass
from xml.sax.saxutils import escape, quoteattr

from .soda_cutout import CutoutAxes

DATALINK_STANDARD = "ivo://ivoa.net/std/DataLink#links-1.1"
SODA_SYNC = "ivo://ivoa.net/std/SODA#sync-1.0"
SODA_ASYNC = "ivo://ivoa.net/std/SODA#async-1.0"

# DataLink 1.1 required columns, in the standard's order.
_FIELDS = (
    ('name="ID" ID="primaryID" datatype="char" arraysize="*" ucd="meta.id;meta.main"'),
    ('name="access_url" datatype="char" arraysize="*" ucd="meta.ref.url"'),
    ('name="service_def" datatype="char" arraysize="*" ucd="meta.ref"'),
    ('name="error_message" datatype="char" arraysize="*" ucd="meta.code.error"'),
    ('name="description" datatype="char" arraysize="*" ucd="meta.note"'),
    ('name="semantics" datatype="char" arraysize="*" ucd="meta.code"'),
    ('name="content_type" datatype="char" arraysize="*" ucd="meta.code.mime"'),
    ('name="content_length" datatype="long" unit="byte" ucd="phys.size;meta.file"'),
)


@dataclass
class Link:
    """One row of the links table."""

    id: str
    semantics: str = ""
    access_url: str = ""
    service_def: str = ""
    error_message: str = ""
    description: str = ""
    content_type: str = ""
    content_length: int | None = None


def _td(value: object) -> str:
    return "<TD/>" if value in (None, "") else f"<TD>{escape(str(value))}</TD>"


def _row(link: Link) -> str:
    cells = (link.id, link.access_url, link.service_def, link.error_message, link.description,
             link.semantics, link.content_type, link.content_length)
    return "<TR>" + "".join(_td(c) for c in cells) + "</TR>"


def _interval(name: str, unit: str, ucd: str, bounds: tuple[float, float] | None, text: str) -> str:
    values = (f'<VALUES><MIN value="{bounds[0]!r}"/><MAX value="{bounds[1]!r}"/></VALUES>'
              if bounds is not None else "")
    return (f'<PARAM name="{name}" datatype="double" arraysize="2" xtype="interval" unit="{unit}" '
            f'ucd="{ucd}" value="">'
            f'<DESCRIPTION>{escape(text)}</DESCRIPTION>{values}</PARAM>')


def input_params(axes: CutoutAxes) -> str:
    """The ``inputParams`` GROUP of a SODA cutout descriptor for one dataset.

    A parameter the dataset cannot use is left out (POS on data without
    celestial axes, BAND without a spectral axis), so a client does not offer
    it; POL is always there, with the states present (``I`` for data without
    a STOKES axis).
    """
    params = ['<PARAM name="ID" datatype="char" arraysize="*" ucd="meta.ref.url;meta.curation" '
              'ref="primaryID" value=""/>']
    if axes.celestial:
        params += [
            '<PARAM name="POS" datatype="char" arraysize="*" ucd="pos.outline;obs" value="">'
            '<DESCRIPTION>CIRCLE, RANGE or POLYGON, ICRS degrees</DESCRIPTION></PARAM>',
            '<PARAM name="CIRCLE" datatype="double" arraysize="3" xtype="circle" unit="deg" '
            'ucd="pos.outline;obs" value=""><DESCRIPTION>ICRS centre and radius</DESCRIPTION></PARAM>',
            '<PARAM name="POLYGON" datatype="double" arraysize="*" xtype="polygon" unit="deg" '
            'ucd="pos.outline;obs" value=""><DESCRIPTION>ICRS vertices (cut to their bounding box)'
            '</DESCRIPTION></PARAM>',
        ]
    if axes.band_m is not None:
        params.append(_interval("BAND", "m", "em.wl;stat.interval", axes.band_m,
                                "Wavelength interval (vacuum), metres"))
    params.append(_interval("TIME", "d", "time.interval;obs.exposure", axes.time_mjd,
                            "MJD interval, checked against the observation time"))
    options = "".join(f'<OPTION value="{escape(state)}"/>' for state in axes.pol)
    params.append('<PARAM name="POL" datatype="char" arraysize="*" ucd="meta.code;phys.polarization" '
                  f'value=""><DESCRIPTION>Polarization states to extract</DESCRIPTION>'
                  f'<VALUES>{options}</VALUES></PARAM>')
    return '<GROUP name="inputParams">' + "".join(params) + "</GROUP>"


def service_descriptor(resource_id: str, standard_id: str, access_url: str, axes: CutoutAxes,
                       description: str) -> str:
    """One descriptor; ``description`` must differ between the descriptors of a
    document, so a client can tell them apart (STILTS datalinklint warns otherwise)."""
    return (f'<RESOURCE type="meta" utype="adhoc:service" ID={quoteattr(resource_id)} '
            f'name={quoteattr(resource_id)}><DESCRIPTION>{escape(description)}</DESCRIPTION>'
            f'<PARAM name="standardID" datatype="char" arraysize="*" value={quoteattr(standard_id)}/>'
            f'<PARAM name="accessURL" datatype="char" arraysize="*" value={quoteattr(access_url)}/>'
            f'{input_params(axes)}</RESOURCE>')


def error_votable(message: str) -> str:
    """A request-level DataLink error (DALI): QUERY_STATUS ERROR, with the
    (empty) links table a DataLink client still expects to find."""
    return _document([], [], status=f'<INFO name="QUERY_STATUS" value="ERROR">{escape(message)}</INFO>')


def links_votable(links: list[Link], descriptors: list[str]) -> str:
    """The DataLink response document: the links table, then the descriptors."""
    return _document(links, descriptors)


def _document(links: list[Link], descriptors: list[str], *, status: str = "") -> str:
    fields = "".join(f"<FIELD {f}/>" for f in _FIELDS)
    rows = "".join(_row(link) for link in links)
    return ('<?xml version="1.0" encoding="UTF-8"?>\n'
            '<VOTABLE version="1.3" xmlns="http://www.ivoa.net/xml/VOTable/v1.3">'
            f'<RESOURCE type="results">{status}<INFO name="standardID" value="{DATALINK_STANDARD}"/>'
            f'<TABLE>{fields}<DATA><TABLEDATA>{rows}</TABLEDATA></DATA></TABLE></RESOURCE>'
            + "".join(descriptors) + "</VOTABLE>")
