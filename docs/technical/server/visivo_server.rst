Introduction
============
VisIVO Server is a suite of software tools for creating customized views of 3D renderings from astrophysical data tables. Their defining characteristic is that no fixed limits are prescribed regarding the dimensionality of data tables input for processing, thus supporting very large scale datasets.

VisIVO Server consists of three core components: :doc:`/server/visivo_importer`, :doc:`/server/visivo_filter` and :doc:`/server/visivo_viewer` respectively. Their functionality and usage is described in the following sections.

To create customized views of 3D renderings from astrophysical data tables, a two-stage process is employed. First, VisIVO Importer is utilized to convert user datasets into `VisIVO Binary Table`_ (VBT). Then, VisIVO Viewer is invoked to display customized views of 3D renderings.

As an example, consider displaying views from only three columns of an astrophysical data table supplied in ascii form, say col_1, col_2 and col_3, by using the commands

.. code-block:: console

    $ VisIVOImporter --fformat ascii UserDataSet.txt
    $ VisIVOViewer -x col_1 -y col_2 -z col_3 --scale --glyphs pixel VBT.bin

VisIVOServer is distributed with GNU General Public License v2.0 License for NON COMMERCIAL use.

VisIVOServer source code is on `GitHub <https://github.com/VisIVOTeam/VisIVOServer>`_.


Installation
-------------------
To install VisIVO Server, follow the instructions indicated in the INSTALL.md file in the GitHub repository https://github.com/VisIVOLab/VisIVOServer. 

VisIVO Binary Table
-------------------
A VisIVO Binary Table (VBT) is a highly-efficient data representation used by VisIVO Server internally. A VBT is realized through a header file (extension ``.bin.head``) containing all necessary metadata, and a raw data file (extension ``.bin``) storing actual data values.

For example, the header may contain information regarding the overall number of fields and number of points for each field (for point datasets) or the number of cells and relevant mesh sizes (for volume datasets). The raw data file is typically a sequence of values, e.g. all X followed by all Y values.


Header
^^^^^^
The header file contains the following fields:

.. code-block:: none

    float | double
    n1
    n2 [ GeoX GeoY GeoZ DX DY DZ ]
    little | big
    X
    Y
    Z
    Vx
    Vy
    Vz

Where:

- float | double
    It is the data type of the storage variables used.
- n1
    It denotes the number of columns (fields) in the VBT (e.g. 6).
- n2
    It denotes the number of rows in the VBT (e.g. 262144).
- GeoX GeoY GeoZ DX DY DZ
    They are employed only if the VBT represents volumetric datasets.
    
    In that case GeoX, GeoY and GeoZ represent the mesh geometry, while DX, DY and DZ represent the x, y and z size of volumetric cells (e.g. 64 64 64 1.0 1.0 1.0).
- little | big
    It denotes the endianism employed in the VBT. After this field there exist n1 rows that indicate the VBT columns as positions (X, Y, Z) and velocities (Vx, Vy, Vz).
