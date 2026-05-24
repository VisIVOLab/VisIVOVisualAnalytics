VisIVO Viewer
=============
VisIVOViewer creates views from the input data file. The input data file must be in the VBT format. The input data file must fit the available RAM.

VisIVOViewer produces five png images. The first four default images are generated with the following fixed values:

.. code-block:: none

    Azimuth     0   90  0   45
    Elevation   0   0   90  45
    Zoom        1

The last image is given with values fixed by the user.

.. note:: The camera of the Viewer at the default position (Azimuth=0, Elevation=0) is looking the box from the top (z plane).

To get a general help:

.. code-block:: console

    $ VisIVOViewer --help

To run the Viewer:

.. code-block:: console

    $ VisIVOViewer <options> inputFile


Global options
--------------

--out <filename>
    Default output filenames are :file:`VisIVOServerImage0.png`, :file:`VisIVOServerImage1.png`, ...
    
    .. note:: The output name is always completed with ``0.png``, ``1.png``, ``2.png`` and ``3.png`` if default images are produced. The output file name (image) is completed with ``.png`` extension.
--nodefault
    Default images are not created.
--cycle <filename>
    (Optional) VisIVO Viewer will produce a sequence of images reading data of azimuth elevation and zooming from the file given with this parameter. This option will prevent the production of default images while the camera position and zoom factor of the line command will be ignored.
    
    The file for the cycle must contain three ascii values for each row. Blank line or rows with less than three values will be ignored. VisIVO will produce one image for each valid row in the file. This file can be created with the tool VisIVOUtils. The output filenames will have the same root-name as those given in --out parameter plus a progressive number (with six digit) starting from 0 (or the value given in the --cycleoffset option), with png file extension. A new file listing all images is created. The file has the fixed name :file:`VSCycleImage#.txt` in the output director, where # is the value of --cycleoffset option.

    The cycle file for camera position must be of four types:

    * Four values for each line: Azimuth, Elevation, Zoom and Roll;
    * Seven values for each line: Azimuth, Elevation, Zoom, Focal Point (three values) and Roll;
    * Eight values for each line: Zoom, Camera Position (three values), Focal Point (three values) and Roll;
    * Ten values for each line: Azimuth, Elevation, Zoom, Camera Position (three values), Focal Point (three values) and Roll;
    
    The keyword NULL is accepted for Camera position and/or Focal Point and/or Roll. In this case the previous camera setting is maintained. The NULL keyword cannot be used for Azimuth, Elevation and Zoom.

    Examples:

    Azimuth, Elevation, Zoom and Roll

    .. code-block:: none

        0.0 0.0 1.0 0
        .....
        60.0 10.0 1.0 NULL
        60.0 10.0 1.2 NULL
        60.0 10.0 1.4 NULL
        60.0 10.0 1.6 NULL
    
    Azimuth, Elevation, Zoom, Focal Point and Roll

    .. code-block:: none

        0.0 0.0 1.0 35 35 35 0
        .....
        60.0 10.0 1.0 20 20 20 0
        60.0 10.0 1.2 20 20 20 0
        60.0 10.0 1.4 20 20 20 0
        60.0 10.0 1.6 20 20 20 0
    
    Zoom, Camera Position, Focal Point and Roll

    .. code-block:: none

        1.0 35 35 200 35 35 35 0
        .....
        1.0 35 35 70 NULL NULL NULL 60
    
    Azimuth, Elevation, Zoom, Camera Position, Focal Point and Roll

    .. code-block:: none

        0 0 1.0 35 35 200 35 35 35 0
        10 0 1.0 35 35 200 35 35 35 0
        10 0 1.5 35 35 200 35 35 35 0
    
    .. note:: Fixing the camera position, the azimuth and the elevation are computed starting from the camera position set.

    If slices are required, the cycle file must contain the sequence of positions in the volume of the planes. This file can be created with the tool VisIVOUtils.
--cycleoffset <value>
    (Optional) The value for the progressive number of files produced with the cycle option. The default value is 0.
--cycle_skip_from <value>
    (Optional) Skips the first number of lines in the cycle file.
--cycle_skip_to <value>
    (Optional) Reads up to the given line in the cycle file.
--camazim <double>
    (Optional) Fixes the camera azimuth position from the camera position.
--camelev <double>
    (Optional) Fixes the camera elevation position from the camera position.

    The allowed range for the camera elevation is [-90, 90].
    If the given camera elevation value is out of the valid range, the elevation is set at the boundary. E.g.: --camelev 100 will automatically be changed with --camelev 90.
--zoom <double>
    (Optional) Zooming factor. A value greater than 1 is a zoom-in, a value less than 1 is a zoom-out (--zoom 2.0).
--camfov <value>
    (Optional) Fixes the zooming factor.
--campos <value>
    (Optional) Fixes the camera position in the system coordinate: 3 values (x, y and z) must be given.
--camfp <value>
    (Optional) Fixes the focal point position in the system coordinate: 3 values (x, y and z) must be given. The default focal point is the center of the system. 
--camroll <double>
    (Optional) Fixes the camera roll factor.
--imagesize
    (Optional) Fixes the size of the image. It may assume the following values: small, medium, large. Default value is medium.
--backcolor
    (Optional) Fixes the background color. It may assume one of the following values: yellow, red, green, blue, white, black, cyan, violet. Default value is black.
--onecolor
    (Optional) Fixes the color of points and isosurfaces (ignored if –-color option is given). It may assume one of the following values: yellow, red, green, blue, white, black, cyan, violet. Default color is white.
--color
    (Optional) Uses the palette.
--colortable <name>
    (Optional) Selects the palette (--colortable default) or (--colortable paletteFilename).

    The following predefined palette can be given: default, default_step, efield, glow, gray, min_max, physics_contour, pure_red, pure_green, pure_blue, run1, run2, sar, temperature, tensteps, volren_glow, volren_green, volren_rg, volren_twolevel, all_yellow, all_red, all_green, all_blue, all_white, all_black, all_cyan, all_violet.

    .. note:: If the --colortable option does not contain a predefined palette, VisIVOViewer assumes that an external filename is given as for this parameter. The file must exist in the current directory or the path must be specified. The palette is an ASCII file. The table must contain [Id+] RGB [+A] or Id+HSV+A comma or space separated values. HSV values are converted into RGB. The Id is an integer that represents the number of points in the palette. RGB (or HSV) and A must be given in [0.0, 1.0] range.

    The palette file can have one of the following formats:
    
    * Only RGB values (or HSV) are given. Opacity (A) is equal to 1.0

    .. code-block:: none

        0.2 0.1 0.7
        0.7 0.5 1.0
        0.4 1.0 0.2

    The palette table will have the number of points (3 in this case) equal to the number of rows.

    * RGB+A values (or HSV+A) are given.

    .. code-block:: none

        0.2 0.1 0.7 0.2
        0.7 0.5 1.0 0.8
        0.4 1.0 0.2 1.0
    
    The palette table will have the number of points (3 in this case) equal to the number of rows.

    * Id+RGB+A values (or id+HSV+A) are given.

    .. code-block:: none

        0 0.2 0.1 0.7 0.2
        100 0.7 0.5 1.0 0.8
        299 0.4 1.0 0.2 1.0

    The palette table will have the number of points equal to the last Id+1 (300 in this case).
    
    Intermediate values (not given) are automatically generated with a linear interpolation between the given values. The table must have increasing Ids. Tables with not ordered Ids are discarded. If the starting 0 point is not given (first row) it is assumed (by default) given as follows:

    .. code-block:: none
    
        0 0.0 0.0 0.0 0.0
    
    The first row indicates the table RGB or HSV. In the case of the RGB table this row could not be given.
--colorrangefrom
    (Optional) Sets the lower limit of the palette.
--colorrangeto
    (Optional) Sets the upper limit of the palette.
--stereo
    (Optional) Produces stereoscopic images. May assume the following values:
    
    - RedBlue, it produces an image for use with red-blue glasses;
    - CrystalEyes, it uses frame-sequential capabilities available in OpenGL to drive LCD shutter glasses and stereo projectors; it produces two images with suffixes _r and _l for the Right and Left eyes.
    - Anaglyph, it is a superset of RedBlue mode, but the color output channels can be configured using the anaglyphmask option and the color of the original image can be (somewhat) maintained using anaglyphsat option.
    
    The default colors for Anaglyph mode is red-cyan. Stereoscopic visualization option is ignored if the slice view is required.
--anaglyphsat
    (Optional) Sets the anaglyph color saturation factor. This number ranges from 0.0 to 1.0: 0.0 means that no color from the original object is maintained, 1.0 means all of the color is maintained. The default value is 0.65.
    
    .. note:: Too much saturation can produce uncomfortable 3D viewing.
--anaglyphmask
    (Optional) Sets the anaglyph color mask values. These two numbers are bits mask that control which color channels of the original stereo images are used to produce the final anaglyph image. The first value is the color mask for the left view, the second the mask for the right view. If a bit in the mask is on for a particular color, that color is passed on to the final view; if it is not set, that channel for that view is ignored. The bits are arranged as r, g, and b, so r = 4, g = 2, and b = 1. By default, the first value (the left view) is set to 4, and the second value is set to 3.
--showlut
    (Optional) visualizes the colorbar.
--showbox
    (Optional) visualizes the box.
--showaxes
    (Optional) visualizes the axes.
--cliplarge
    (Optional) Fixes the clipping plane from 0 to 1.0e+13. The cliplarge option can be used if a black image is obtained. To fix specific range use the cliprange option. This option is ignored if cliprange is given.
--cliprange <value>
    (Optional) Fixes the clipping plane in the system coordinate: 2 values must be given (i.e. 0.0 1.0e+4). The user can set this values using the statistic filter and checking the user data value in the field of view.
--history
    (Optional) create an XML file which contains the history of operations performed. Default create :file:`hist.xml` file.
--historyfile <filename>
    (Optional) Change default history file name and or directory.


Parameter file
--------------
This alternative command allows VisIVOViewer to read all options from a parameter file. Lines starting with # are comments.

An example of this file is the following:

.. code-block::

    ############## VisIVO SECTION
    ######## Sect 1 General
    volume=no
    vector=no
    input=u2.bin
    out=outFilename
    showbox=yes
    showaxes=no
    imagesize=medium
    #cycle=cicleFilename
    #cycleoffset=0
    #cycle_skip_from=0
    #cycle_skip_to=0
    ######## Sect 2 Points and vectors
    x=X
    y=Y
    z=Z
    #vx=VX
    #vy=VY
    #vz=VZ
    #scale=yes
    #######
    ########### Sect 3 Volume
    #vrendering=yes
    #isosurface=no
    #slice=no
    #shadow=no
    #vrenderingfield=ColumnName
    #slicefield=ColumnName
    #sliceplane=x
    #sliceposition=0
    #sliceplanenormal= 1 1 1
    #sliceplanepoint= 10 10 10
    #isosurfacefield=ColumnName
    #isosurfacevalue=120
    #wireframe=no
    #isosmooth=none
    ################# Sect 4 Camera
    camazim=20
    camelev=20
    campos= 35 35 200
    camfp= 35 35 35
    zoom=1.5
    nodefault=yes
    #largeimage=no
    ################# Sect 5 Colour
    color=yes
    colorscalar=X
    colortable=default
    #colorrangefrom=0
    #colorrangeto=100
    #onecolor=white
    #backcolor=black
    #showlut=yes
    opacity=0.666
    #logscale=no
    ########### Sect 5 Glyphs
    #glyphs=sphere
    #scaleglyphs=no
    #radius=1.0
    #radiusscalar=ColumnName
    #height=1.0
    #heightscalar=ColumnName
    #vectorline=yes
    #vectorscalefactor=1.0
    #vectorscale=1

Visualization
-------------
The following kinds of visualizations are available:

.. contents::
    :local:


Data Points
^^^^^^^^^^^
VisIVO Viewer creates data points views from the input data file. The Input data file must be in VBT format. The input data file must fit the available RAM.

Options:

-x <field>
    (Optional) Selects the first coordinate.
-y <field>
    (Optional) Selects the second coordinate.
-z <field>
    (Optional) Selects the third coordinate.
--scale
    (Optional) Enables data normalization. It always allows you to visualize a cubic region even if the coordinates system has different scales. The field names containing X,Y,Z or RA,DE and Mag are assumed to be default values for the x y z system, or the first three table columns, if these options are not given.
    
    .. note:: It is strongly recommended to fix these parameters to prevent unpredictable behavior.
--colorscalar <field>
    (Optional) Selects the field for the palette.
--logscale
    (Optional) Uses the logarithmic scale for the palette. If the select field has values less than or equal to 0 this option is ignored and the linear scale will be used.
--glyphs <name>
    (Optional) Data points are displayed with different geometrical form. The following forms are available: pixel, sphere, cone, cylinder, cube. This option has no effect if the data point number is more than 1000.
--radius <vaulue>
    (Optional) Radius of the geometrical form.
--height <value>
    (Optional) Height of the geometrical form (where applicable).
--opacity <double>
    (Optional) Data points opacity. Default value 0.66.
--opacityTF <three double values>
    (Optional) Data smoothed points opacity representation. They fix the curve slope for opacity transfer function when smoothed representation is given. The three values must not be negative. Suggested ranges are [3-10] [1-5] [2-5]. Default values are 5.0 3.0 2.5.
--scaleglyphs
    (Optional) Enables the geometrical form to be scaled with a scalar field.
--scenario
    (Optional) In a smoothed representation it gives the colors for data point. Each scenario is represented by a string name. The active current scenario is called etna. This is the default value.
--radiusscalar <field>
    (Optional) Sets the scalar field for radius scaling.
--heightscalar <field>
    (Optional) Sets the scalar field for height scaling.

Examples:

Palette usage

.. code-block:: console

    $ VisIVOViewer --x X --y Y --z Z --color --colorscalar scalar0 --colortable temperature --logscale /home/user/inputFile.bin

Normal glyphs

.. code-block:: console

    $ VisIVOViewer --x X --y Y --z Z --glyphs cone --radius 1 --height 2 /home/user/inputFile.bin

Scaled glyphs

.. code-block:: console

    $ VisIVOViewer --x X --y Y --z Z --glyphs cone --scaleglyphs --radiusscalar scalar0 --heigthscalar scalar1 /home/user/inputFile.bin


Volumes
^^^^^^^
VisIVOViewer creates a volume view of data points from the input data file that contains a volume. The input data file must be in VBT format and must have the number of mesh elements on each dimension. The input data file must fit the available RAM.

A volume can be visualized with the volume rendering technique, with an isosurface or with slices. A color table must be given. The default color table will be used if the colortable option is not given.

Specific volume options:

--volume
    (Optional) Enables volume visualization.
--vrendering
    (Optional) Enables volume rendering view. The volume rendering view is the default when --volume is given.
--vrenderingfield <field>
    Sets the scalar to be represented in the view.
--shadow
    (Optional) Enables shadow view in the rendering view.
--autorange
    (Optional) Enables auto range feature, which allows to get an automatically computed view range for the image.
--autorangemin <value>
    (Optional) Sets the minimum value of view range for the image, it is ignored if --autorange option is not enabled.
--autorangemax <value>
    (Optional) Sets the maximum value of view range for the image, it is ignored if --autorange option is not enabled.
        
Example

.. code-block:: console

    $ VisIVOViewer --volume --vrendering --vrenderingfield density –colortable temperature /home/user/inputFile.bin

Specific isosurface options:

--isosurface
    (Optional) Enables isosurface view.
--isosurfacefield <field>
    (Optional) Sets the scalar to be represented in the view.
--isosurfacevalue <field>
    (Optional) Fixes the isocontur value: from 0 to 255.
--wireframe
    (Optional) Visualizes the isosurface with wireframe.
--isosmooth
    (Optional) Smoothes the isosurface visualization. It may assume the following values: none (default), medium, high.
    
Example

.. code-block:: console

    $ VisIVOViewer --volume --isosurface --isosurfacefield density --isosurfacevalue 200 /home/user/inputFile.bin

Specific slider options:

--slice
    (Optional) Enables slice view.
--slicefield <field>
    (Optional) Sets the scalar to be represented in the slice view.
--sliceplane <plane>
    (Optional) Sets the plane to be represented in the view. It must be one of the following: x, y, z. The camera is always positioned in front of the plane.
--sliceposition <position>
    (Optional) Sets the plane coordinate position to be represented in the view. It must be an integer value from 0 to the maximum number of cells in the selected direction. Ignored if cycle option is given.

VisIVOViewer can also visualize oblique planes. In this case sliceplane and sliceposition options must not be given. The camera is positioned using azimuth and elevation options.

--sliceplanepoint
    (Optional) Sets the three coordinates of a point in the plane. It is ignored in case of cycle file.
--sliceplanenormal
    (Optional) Sets the three coordinates of a point belonging to the normal axes to the slice. The sliceplanepoint and the sliceplanenormal fix the points and anthe axis in the space. The slice is normal to this axis and the point in sliceplanepoint is a point of this plane. It is ignored in case of cycle file.

*Important Remarks*. One of the following options must be specified: sliceplane, sliceplanenormal and/or sliceplanepoint. If sliceplane is selected orthogonal slices will be produced. If sliceplane is not given but sliceplanenormal and/or sliceplanepoint are given, generic slices will be produced. In case of cycle the specific values of sliceposition, sliceplanenormal and sliceplanepoint are ignored and the cycle file values will be used, even if the options must be given to select the type of slice visualization.

.. note:: The stereoscopic visualization is ignored in case of slice.
.. note:: Cycle can be given for Orthogonal Normal planes (x, y or z). In this case the cycle file must contain a sequence of integers (one for each row) inside the volume range (e.g 0-64).
.. note:: Cycle can be given for point-planenormal slice. In this case the cycle file must contain a sequence of six values: three point coordinates and three plane normal coordinates. Lines with less than 6 values are ignored. In this case the showbox option is recommended.

Example:

.. code-block:: console

    $ VisIVOViewer --volume -–slice --slicefield density --sliceplane x –sliceposition 3 –color --colortable default /home/user/inputFile.bin


Vectors
^^^^^^^
VisIVOViewer creates a view of vectors created from the input data file that contains data points. The input data file must fit the available RAM.

Options:

--vector
    (Optional) Enables vector visualization.
--x <field>
    (Optional) First component of the vector application point.
--y <field> 
    (Optional) Second component of the vector application point.
--z <field>
    (Optional) Third component of the vector application point.
--vx <field>
    (Optional) First component of the vector.
--vy <field>
    (Optional) Second component of the vector.
--vz <field>
    (Optional) Third component of the vector.
--colorscalar <field>
    (Optional) Selects the field of the VBT to be used for the palette. The vectors are displayed with the color palette based on the value of the active scalar given in this option. If this option is not given, the palette is based on is the magnitude of the vector. This option set the active scalar.
--vectorline
    (Optional) Enables the vector representation with a line. Default is arrows.
--vectorscalefactor <field>
    (Optional) Scale factor for vector representation.
--vectorscale <field>
    (Optional) Assumes the following values. Value 0: the scale of the vector dimension is given by the active scalar (colorscalar option). Value 1: the scale of the vector dimension is given by the vector magnitude. Value -1 (default): the vectors are not scaled.

Example:

.. code-block:: console

    $ VisIVOViewer --x X --y Y --z Z -–vx Vx –-vy Vy –-vz Vz --color --colorscalar scalar0 --colortable temperature --vectroscalefactor 1.3 --vectorscale 0 /home/user/inputFile.bin
