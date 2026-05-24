VisIVO Utils
============
VisIVO Utilities is a tool that creates intermediate data that will be used by the other components of VisIVO Server. These data could consist of a sequence of values or files extracted from VisIVO Binary Tables.

To get general help:

.. code-block:: console

  $ VisIVOUtils --help
  
To get a specific utility help:

.. code-block:: console

  $ VisIVOUtils --op utilitycode --help
  
To run the utility:

.. code-block:: console
  
  $ VisIVOUtils --op utilitycode <options>

Utilities
---------
The following utilities are available:

.. contents::
    :local:
    :depth: 1
    
Create path
^^^^^^^^^^^
This utility append or creates an ascii files containing 4, 7 or 8 values for each row for the camera position. The file can be used by VisIVOViewer --cycle to produce a sequence of png images to be mounted in a movie.

The camera position, focal point and roll, when not specified, will contain NULL that will allow VisIVO Viewer to maintain the last used setting.

Usage:

.. code-block:: console

  $ VisIVOUtils --op createpath --type value [--azimuth from to] [--elevation from to] [--zoom from to] [--zoomend [stepframe]] [--campos from to] [--camfp from to] [--camroll from to] [--framesec value] [--length value] [--out filename] [--help]
  
Options:


--type
  | 0 Create path for azimuth, elevation, zoom and roll. Default value.
  | 1 Create path for azimuth, elevation, zoom, focal point and roll.  
  | 2 Create path for zoom, camera position, focal point and roll.
  | 3 Create path for azimuth, elevation, zoom, camera position, focal point and roll.

--azimuth
  Movement from to. Default values 0.0 and 0.0.

--elevation
  Movement from to. Default values 0.0 and 0.0. Valid range [-90,90]. Values outside this interval are automatically set to the near extreme: Ex.: ``--elevation -85 100`` will be modified with ``–-elevation -85 90``.
  
--zoom
  Zoom from to. Default values 1.0 and 1.0. A zooming factor <1 represents a zoom in a zooming factor >1 represent a zoom out. Negative value are ignored.

--zoomend
  The zoom is given at the end. The value step-frame represent the step for zooming . Default step-frame is 0.2 If this option is given priority with zoom will be ignored. The final zooming is added to global the length.

--campos
  Movement from to. Three vale for starting point and three value for ending point are expected.

--camfp
  Movement from to. Three vale for starting point and three value for ending point are expected.

--camroll
  Movement from to. Three vale for starting point and three value for ending point are expected.

--framesec
  Number of frame values for each second. Default value is 10.

--length
  Value in seconds. Default value is 10 sec.

--out
  Output filename. Default filename cycle.par. The file is opened in append mode.
  
Example:

.. code-block:: console

  $ VisIVOUtils --op createpath --type 0 --azimuth 0.0 60. --elevation 0.0 10.0 --zoom 1.0 1.5 --zoomend --length 20 --out my_cycle.par
  
The utility produce 10 values (default value) for each second. The file :file:`my_cycle.par` will contain: azimuth, elevation, zooming and roll:

.. code-block::

  0.0 0.0 1.0 NULL
  ......
  60.0 10.0 1.0 NULL
  60.0 10.0 1.2 NULL
  60.0 10.0 1.4 NULL
  60.0 10.0 1.6 NULL
  
Orthogonal Slices
^^^^^^^^^^^^^^^^^
This utility append or creates an ascii file containing the slice poisition in the volume table. The file can be used by VisIVOViewer --cycle to visualize slices and to produce a sequence of png images to be mounted in a movie.

Usage:

.. code-block:: console
  
  $ VisIVOUtils --op orthoslices --pos from to [--xplane] [--yplane] [--zplane] [--step stepvalue] [--out filename] [--help] [--file inputFile.bin]
  
Options:

--pos
  Sets the slice position from-to in the volume. Values outside the volume size are ignored.
  
--xplane
  Sets the direction x to be considered. Default is x.

--yplane
  Sets the direction y to be considered. It is ignored if --xplane is given.

--zplane
  Sets the direction z to be considered. It is ignored if --xplane or --yplane is given.
  
--step
  Step increment for slice position (integer). Default value 1.

--out 
  output filename. Default filename cycle.par. The file is opened in append mode.

--file
  (optional) Input Volume table.

Example:

.. code-block:: console

  $ VisIVOUtils --op orthoslices --pos 0 64 --step 1 --out my_cycle.par --file inputFile.bin
  
The utility produces 64 values as follows:

.. code-block::

  0
  1
  2
  ...
  64
  
Generic Slices
^^^^^^^^^^^^^^
This utility append or creates a file with six columns. The point position (plane point) has increased (decreased) of step_size for n steps. The plane point is moved along the normal axis. The product step*size determines the movement of the plain point. If step*size is equal to 1, at the end the plane point will be at the same point of the normal point.

The file can be used by VisIVOViewer ``--cycle`` to visualize generic slices and to produce a sequence of png images to be mounted in a movie.

Usage:

.. code-block:: console
  
  $ VisIVOUtils --op genericslices --point x y z --normal x y z --step n [--size step_size] [--movedown] [--out filename] [--help]
  
Options:

--point
  The three coordinates of a point in the plane.
  
--normal
  The three coordinates fixing the normal axis to the plane.
  
--step
  Number (int) of generated new point positions along the normal axis.

--size
  Value of increased (decreased) point coordinates. Default value 1.
  
--movedown
  The plane point is moving to the opposite side of the normal point.
  
--out
  Output filename. Default filename cycle.par. The file is opened in append mode.
  
Example:

.. code-block:: console
  
  $ VisIVOUtils --op genericslices --point 1 1 1 --normal 10 10 10 --step 20 --size 0.05 --out cyclefile

The utility produces 21 values (including the starting point) as follows:

.. code-block::

  1 1 1 10 10 10
  1.45 1.45 1.45 10 10 10
  1.9 1.9 1.9 10 10 10
  2.35 2.35 2.35 10 10 10
  2.8 2.8 2.8 10 10 10
  3.25 3.25 3.25 10 10 10
  3.7 3.7 3.7 10 10 10
  4.15 4.15 4.15 10 10 10
  4.6 4.6 4.6 10 10 10
  5.05 5.05 5.05 10 10 10
  5.5 5.5 5.5 10 10 10
  5.95 5.95 5.95 10 10 10
  6.4 6.4 6.4 10 10 10
  6.85 6.85 6.85 10 10 10
  7.3 7.3 7.3 10 10 10
  7.75 7.75 7.75 10 10 10
  8.2 8.2 8.2 10 10 10
  8.65 8.65 8.65 10 10 10
  9.1 9.1 9.1 10 10 10
  9.55 9.55 9.55 10 10 10
  10 10 10 10 10 10

Load History
^^^^^^^^^^^^

This utility, starting from a history xml file creates a bash script for re-execution.

Usage:

.. code-block:: console

  $ VisIVOUtils --op loadhistory --file <hist.xml> [--help]

Options:

--out
  Output filename. Default filename VisIVO.sh

--file
  Input History file.
  
Example:

.. code-block:: console

  $ VisIVOUtils --op loadhistory --file hist.xml
  
The utility produces the script :file:`VisIVO.sh` from the file :file:`hist.xml`.

Text Column
^^^^^^^^^^^
Starting from an ascii file file, extract the value of a column as string.

Usage:

.. code-block:: console

  $ VisIVOUtils --op textcol --file <table.ascii> --colname <column_name> [--help]
  
Options:

--colname
  The name of the requested column.

--out
  Output filename. Default filename VisIVO.sh

--file
  Input ASCII file.

Example:

.. code-block:: console

  $ VisIVOUtils --op textcol --file table.txt --colname X
  
The utility extracts the column X from file :file:`table.txt`.
