VisIVO Importer
===============
VisIVO Importer converts user-supplied datasets into a VBT. VBTs are used by VisIVO Filter for data processing and VisIVO Viewer for visualization. The conversion is independent of data dimensionality.

General VisIVO Importer Syntax:

.. code-block:: console

    $ VisIVOImporter --flag1 --flag2 ... --flagn FileName

VisIVO Importer General Options:

--fformat
    Specifies the format of input datasets.
--out <name>
    (Optional) Output FileName. The path specified with this flag is the VisIVO Importer output directory.
    
    If no path is specified the VisIVO Importer output directory is the current directory, and the default name for OutputFileName is :file:`VisIVOServerBinary.bin`.
--volume
    (Optional) Used to create volumes.
--compx <value>, --compy <value>, --compz <value>
    (Optional) Used for entering geometry for volumetric datasets.
    
    If data size fits in a cubic mesh, values are computed automatically.
--sizex <value>, --sizey <value>, --sizez <value>
    (Optional) Used for specifying volumetric cell dimensions; the default values are 1.0, 1.0 and 1.0 respectively.
--userpwd <username:password>
    (Optional) Used to prescribe a username and password for accessing remote files.
    
    Ambiguous characters for the shell (i.e. $ >, < etc..) must be given with escape character “\”. For example ``guest$09`` must be given as ``guest\$09``.

    .. note:: Escape characters MUST NOT BE GIVEN using the parameterFile.
--binaryheader <headerfilename>
    (Optional) Used to specify the file name of the header of VBTs; this flag is ignored in case of other formats.
--missingvalue <value>
    (Optional) Used to set the missing data to a fixed value. If not present the default value is: -1.0918273645e+23.
--textvalue <value>
    (Optional) Used to set the textual data to a fixed value. If not present a default value is given: -1.4536271809e+15.
--bigendian
    (Optional) Used only for big endian Gadget and FLY input files, otherwise by default this flag is set to little endian.
--double
    (Optional) Used only for the double data type of FLY input files, otherwise by default this flag is set to float.
--npoints <value>
    (Optional) Used only for FLY input files to specify the number of data points.
--history
    (Optional) Create an XML file which contains the history of operations performed.
--historyfile <filename>
    (Optional) Change output history file name. Default: :file:`hist.xml`.

The FileName is the local (or remote) file containing the data to be converted into a VBT. If FileName starts with ``http://``, ``ftp://`` or ``sftp://`` the remote file is downloaded automatically. However if the --userpwd option is specified, the prescribed username and password are employed for remote access. The sftp syntax requires the remote directory where data are located.

.. note:: sftp is not allowed if VisIVO is compiled with the LIGHT option. The server must have libcurl with ssl support to enable the sftp functionality and VisIVO must be compiled without the LIGHT option.

All downloaded files are copied temporarily into the directory given in --out option and are deleted automatically at the end of the import process. Under the current file directory, the file :file:`DownLoadedFilename_VisIVO_List` contains information on all download operations. If the fformat option is binary VisIVOImporter will attempt to download two remote files, a binary table (given as remote filename) and its associated header file (same name +``.head`` extension).


Parameter file
--------------
Alternatively, to run the importer with options specified in the parameter file:

.. code-block:: console

    $ VisIVOImporter <parameterFile>

Lines starting with # are comments.

Options with one or more parameters must be all specified after the “=” sign in the same line (e.g. field=X Y Z).

Options that do not require parameters must be given with “true” keyword (e.g. double=true, bigendian=true).

The input filename has the keyword file (ex. file=myInputFilename).

Examples of parameter files are the following:

.. code-block::

    fformat=ascii
    out=outFilename.bin
    file=asciinputFile

.. code-block::

    fformat=votablefast
    out=/home/user/dataNewTable.bin
    missingvalue=0.0
    file=myVOTable.xml

.. code-block::

    fformat=fly
    out=FlyData.bin
    double=true
    npoints=1000
    bigendian=true
    userpwd=myusername:mypassword
    file=http://remotehosts.domain.eu/directory/InputDataFile


Formats
-------
The following file formats are supported:

.. contents::
    :local:
    :depth: 1


ASCII
^^^^^
ASCII files are expected to be in tabular form. An ASCII file may contain values for N variables organized in columns. The columns are typically separated by whitespace characters, e.g. spaces or tabs.

The first row of an ASCII file lists the N variables names explicitly. As an example, the command below produces :file:`NewTable.bin`, :file:`NewTable.bin.head` from :file:`ASCIIUserFileName.txt`.

Usage:

.. code-block:: console

    $ VisIVOImporter --fformat ascii --out /home/user/data/NewTable ASCIIUserFileName.txt


CSV
^^^
CSV is a delimited data format that has fields/columns separated by the comma character and records/rows separated by newlines.

Fields that contain a special character (such as comma, newline, or double quote) must be enclosed in double quotes. However, if a line contains a single entry that happens to be the empty string, it may be enclosed in double quotes. If a field's value is a double quote character, this is dealt with by placing another double quote character next to it.

The CSV file format does not require a specific character encoding, byte order, or line terminator format. 

As an example, the command below produces the files :file:`NewTable.bin` and :file:`NewTable.bin.head` from the user-supplied CSV file :file:`CSVUserFileName.txt`.

Usage:

.. code-block:: console

    $ VisIVOImporter --fformat csv --out /home/user/data/NewTable CSVUserFileName.txt

.. note:: The Importer automatically skips all the lines starting with # character. If the first line contains column names starting with #, this character will be removed, and the columns names will be given without it.

BINARY
^^^^^^
The binary format is supposed to be the Internal Binary Table. 

Usage:

.. code-block:: console

    $ VisIVOImporter --fformat binary --out /home/user/data/NewTable BinaryUserFilename.bin

Assuming that a VBT is to be processed, the previous command produces :file:`NewTable.bin` and :file:`NewTable.bin.head` from :file:`VBTUserFileName.bin`. Note that the files :file:`VBTUserFileName.bin` and :file:`VBTUserFileName.bin.head` must be (either local or remote) existing files. Also, if the --binaryheader option is prescribed, a header file with the specified filename must exist. In case this header file resides remotely, a complete copy is created within the VisIVO Server output directory.

.. note:: This command is useful for changing the endianism of a binary table. An input big endian table is transformed in a little endian table if system where VisIVOImporter is running is a little endian system and viceversa.

FLY
^^^
FLY is code that uses the tree N-body method, for three-dimensional self-gravitating collisionless systems evolution. FLY is a fully parallel code based on the tree Barnes-Hut algorithm; periodical boundary conditions are implemented by means of the Ewald summation technique.

FLY is based on the one-side communication paradigm for sharing data among processors, accessing remotely private data without synchronism.
The FLY output format is a binary sequence of values of n data points as follows: X1,Y1,Z1,X2,Y2,Z2 ,.... Xn,Yn,Zn, Vx1,Vy1,Vz1, Vx2,Vy2,Vz2, .... Vxn,Vyn,Vzn. 

As an example, the command below produces :file:`NewTable.bin` and :file:`NewTable.bin.head` from :file:`FLYUserFile` which is a FLY file using double data types, containing a total of 2000000 data points.

Usage:

.. code-block:: console

    $ VisIVOImporter --fformat fly --out /home/user/data/NewTable --double --npoints 2000000 FLYUserFile

The FLY format also allows the download of elements given in a descriptor file (.desc extension):

.. code-block::

    flyDesc             (type of the Fly descriptor)
    2m_test             (ID)
    double              (data type)
    time                (ID for snapshot sequence)
    2097152             (number of points on each snapshot)
    50 50 50            (box dimension in the proper unit)
    l                   (“l” or “b” for data endianism)
    0.0 out_scdm_0.0000 (time tag and snapshot filename sequence)
    1.0 out_scdm_1.0000
    2.0 out_scdm_2.0000

In this case the flags --npoints and --double must no be given as values are read automatically from the descriptor file. Each listed file (out_scdm_# in this case) produces a VBT.

.. code-block:: console

    $ VisIVOImporter --fformat fly --out /home/user/data/NewTable FLYUserFile.desc
    
.. note:: The names of output files will be determined by using the --out parameter concatenated first by a "_" and then by the listed filename. As an example, if --out /tmp/pippo is prescribed, output filenames will be :file:`/tmp/pippo_out_scdm_0.000.bin` (and one with the extension .bin.head). On the other hand, if --out option is not prescribed, output filenames will be :file:`./VisIVOServerBinaryout_scdm_0.000.bin` (and one with extension .bin.head).

FITS Table
^^^^^^^^^^
The definition of FITS is a codification into a formal standard, by the NASA/Science Office of Standards and Technology (NOST), of the FITS rules (http://fits.gsfc.nasa.gov) endorsed by the IAU. 

FITS supports tabular data with named columns and multidimensional rows. Both binary and ASCII FITS table versions have been specified. The data in a column of a FITS table can be in a different format from the data in other columns. Together with the ability to string multiple header/data blocks together, by using FITS files it is possible to represent entire relational databases. 

As an example, the command below produces :file:`NewTable.bin`, :file:`NewTable.bin.head` from :file:`FITSTableUserFile`.

Usage:

.. code-block:: console

    $ VisIVOImporter --fformat fitstable --out /home/user/data/NewTable FITSTableUserFile
 
FITS Image
^^^^^^^^^^

The Definition of FITS is a codification into a formal standard, by the NASA/Science Office of Standards and Technology (NOST), of the FITS rules endorsed by the IAU. 

FITS image headers can contain information about one or more scientific coordinate systems that are overlain on the image itself. Images contain an implicit Cartesian coordinate system that describes the location of each pixel in the image, but scientific uses generally require working in 'world' coordinates, for example the celestial coordinate system.

As an example, the command below produces :file:`NewTable.bin`, :file:`NewTable.bin.head` from :file:`FITSImageUserFile`.

Usage:

.. code-block:: console

    $ VisIVOImporter --fformat fitsimage --out /home/user/data/NewTable FITSImageUserFile

GADGET
^^^^^^
GADGET is freely-available code for cosmological N-body/SPH simulations on massively parallel computers with distributed memory. GADGET uses an explicit communication model that is implemented with the standardized MPI communication interface. The code can be run on essentially all supercomputer systems presently in use, including clusters of workstations or individual PCs.

VisIVO Importer will produce a VBT for each species in the gadget file supplied. As an example, the command below will produce the files :file:`NewTableHALO.bin`, :file:`NewTableHALO.bin.head`, and :file:`NewTableGAS.bin`, :file:`NewTableGAS.bin.head` from the GADGET file :file:`GadgetUserFile` if we assume that it contains only two species, namely halo and gas particles.

Usage:

.. code-block:: console

    $ VisIVOImporter --fformat gadget --out /home/user/data/NewTable GadgetUserFile
    
HDF5
^^^^
The hierarchical Data Format is a library and multi-object file format for the transfer of graphical and numerical data between computers. It was created by the NCSA, but is currently maintained by The HDF Group.  The freely available HDF distribution consists of the library, command-line utilities, test suite source, Java interface, and the Java-based HDF Viewer (HDFView). 

HDF supports several different data models, including multidimensional arrays, raster images, and tables. Each defines a specific aggregate data type and provides an API for reading, writing, and organizing data and metadata. New data models can be added by the HDF developers or users.

.. code-block:: console

    $ VisIVOImporter --fformat hdf5 --datasetlist dataset1 dataset2 --hyperslab dataset 210,10 100,100 --out /home/user/data/NewTable HDF5UserFile
    
.. code-block:: console

    $ VisIVOImporter --fformat hdf5 --datasetlist vol1 vol2 --hyperslab vol1 0,0,0 10,10,10 vol2 5,5,5 10,10,10 --compx 10 --compy 10 --compz 10 --out /home/user/data/NewTable HDF5UserFile

Options
+++++++

Dataset
~~~~~~~
This importer can be used with the datasetlist option. When the datasetlist option is not given all datasets in the hdf5 will be considered forming the VBT.

.. code-block:: console

    --datasetlist nameOfDatset1 nameOfDatset2 nameOfDatset3 ...
    
If the parameterfile is used the file can contain more rows listing datasets. The parameter file must have only one dataset in the datasetlist parameter. More rows with datasetlist parameter must be given to import more than one dataset.

Hyperslab
~~~~~~~~~
The hyperslab option must be given as follows:

.. code-block:: console

    --hyperslab nameOfDataset offset count
    
where offset and counts are the same of the hdf5 file and must be comma separated values. If count exceeds the size of dataset, it is automatically adjusted up to the end of hyperslab.

If offset and/or count contains lower values than the dataset dimension, not specified offset are put to 0 and count to the dimension of the dataset. But this fact could give some unexpected behaviors and it is strongly recommended giving all parameters of offset and dimension.

The offset and dimension values must be equal to the number of rank. If rank=3 offset/dimension must contain 3 separated comma values (one for each rank) if offset/dimension contains only two values the third value is assumed to be equal to the size of the dataset. 

Example rank=3, dataset dimension (200,200,200). --hyperslab datasetname 10,10 20,20 is considered as --hyperslab datasetname 10,10,200 20,20,200.

The parameter file must have only one hyperslab in the hyperslab parameter. More rows with an hyperslab parameter must be given to describe more than one dataset.

Note in case of volume with more than one hyperslab: users are strongly suggested to givethe same hyperlab extension (count). In case of volume with specified hyperslab the --compx-y-z values are ignored. Datasets with different numbers of rows will produce columns with the number of rows equal to the maximum number of rows. The rows will be pads with missingvalue parameter. 

Datasets with rank greather than 1000 cannot be read. 

Datasets
++++++++
Datasets can represent tables or volumes. If a dataset represents a volume the dataset rank must be equal to 3 and the --volume option must be given.

Tables
~~~~~~
A dataset with rank > 1 will produce different columns in the VBT. If a dataset is a table and it has rank=3 and hyperslab offset=0,0,0 count=15,10,1000, it will produce 15*10 columns each having 1000 elements. The columns names will be datasetname_0_0 datasetname_0_1 datasetname_0_2.... datasetname_14_9.

Volume
~~~~~~ 
The dataset rank must be equal to 3.
If the dataset represents a volume the --volume option must be given. In this case the hyperslab dimension (if given) represents the volume dimension and the --compx --compy -- compz options are ignored.
More datasets can be given, but in this case they must have the same hyperslab dimension (the first hyperslab sets the volume resolution).

Examples
~~~~~~~~
1. Tables

Reading three datasets from a file, the dataset1 has rank = 2, the dataset2 has rank =3 and hyperslabs are specified. The dataset 3 is totally imported.

.. code-block::

    fformat=hdf5
    datasetlist=dataset1
    datasetlist=dataset2
    datasetlist=dataset3
    out=filename_out.bin
    hyperslab=dataset1 20,45,65 32,38,49
    hyperslab=dataset2 30,45,65 32,38,49
    file=myFile.h5

2. Volume 

Reading two datasets (volumes) from a file, each dataset must have rank = 3 and the same hyperslab extension.

.. code-block::

    fformat=hdf5
    datasetlist=vol1
    datasetlist=vol2
    out=filename_out.bin
    hyperslab=vol1 0,0,0 50,45,50
    hyperslab=vol2 100,100,100 50,45,50
    volume=true
    compx=50
    16
    compy=45
    compz=50
    sizex=1.0
    sizey=1.0
    sizez=1.0
    file=myFile.h5
    
MUPORTAL
^^^^^^^^
MUPORTAL files are expected to be in tabular form. They are produced from the experiment Muon Portal. It is an ASCII file containing rows with 10 values space separated. Each row represent an event (muon track). The column names are automatically added by the importer. The first row of the file contain the first event. The columns are typically separated by whitespace characters, e.g. spaces or tabs. 

The 10 columns represent: Event number, X_A Y_B X_C Y_D X_E Y_F X_G Y_H (8 values coordinates in cm at the planes of the system), Pulse energy in GeV/C. As an example, the command below produces :file:`NewTable.bin`, :file:`NewTable.bin.head` from :file:`MuPortal.in`.

Usage:

.. code-block:: console

    VisIVOImporter --fformat muportal --out /home/user/data/NewTable MuPortal.in

.. note:: The Importer automatically skips all lines starting with # character.

RAMSES
^^^^^^
RAMSES files contain many type of data. This importer reads the particles positions only. 
The input is the root filename of a sequence of files (normally equal to the number of processes that generate the ramses output). Each file contains a set of particles (e.g. :file:`part_seq.out00001` .... :file:`part_seq.out00064`). The importer add .outXXXXX where XXXXX are 5 numbers in a sequence from 00001 to the number of processes that is automatically read from files.

The ramses file can have 1, 2 or 3 dimension. The generated output in any case will have 3 dimensions. In case of 1 or 2 dimensions the second and/or third dimension assume the missingvalue (--missingvalue option).

Usage:

.. code-block:: console

    $ VisIVOImporter --fformat ramses --out /home/user/data/NewTable dir/output/part_seq
    
RAW Binary
^^^^^^^^^^
Raw files are simply a binary dump of the memory for data points. The content of the Raw Binary data points file is a sequence of x,y and z coordinate for each point, then a sequence of fields, one scalar for each data point.

General binary file structure:

.. code-block:: 

    1, Y1, Z1
    X2, Y2, Z2
    ..........
    Xn, Yn, Zn
    Field0_1
    Field0_2
    ........
    Field0_n
    Field1_1
    Field1_2
    ........
    Field1_n
    ........
    Fieldm_1
    Fieldm_2
    ........
    Fieldm_n
    
VisIVOImporter reads a descriptor file. More than one raw data file name can be described. The descriptor file has the following structure:

* rawPointDesc
* Variable name
* Variable type
* Time Variable (at the moment not used, but needed in the descriptor file)
* Number of particles
* The size of the box
* Endianism type (b=big endian or l=little endian)
* List of Ids of the data files (a number representing the order or the time) and names of the data files

Example of Descriptor file: 

.. code-block:: 

    rawPointsDesc
    20
    dark
    Float
    time
    130000
    50 50 50
    b
    0.0 16ml_096
    0.5 16ml_104
    1.0 16ml_112

All the files listed in the descriptor file must be given. Each file will be converted and an internal binary table will be created for each listed file.
Output files will have the same name of --out parameter +listedfilename+”.bin” and “.bin.head”. If a filename start with http:// or ftp:// the remote file is downloaded. If the --userpwd option is given the username and password are used for remote access to the file. Downloaded files are temporarily copied in the same directory given in --out option and cleaned at the end of the import phase. The file downLoadedFilename_VisIVO_list in the current directory contains information on the download operations.

Usage:

.. code-block:: console

    $ VisIVOImporter --fformat rawpoints --out /home/user/data/NewTable rawpointsUserFile.desc
    
RAW Grid
^^^^^^^^
Raw files are simply a binary dump of the memory. For volume data, only one quantity is expected to be stored in each file. The content of a volume file is a sequence of values: one value for each mesh point.

VisIVOImporter reads a descriptor file. More than one raw data file name can be described. The descriptor file has the following structure: 

* rawGridDesc
* Variable Name
* Number of spatial dimensions
* Variable type
* Number of cells in the first dimension
* Number of cells in the second dimension
* Number of cells in the third dimension
* Time variable (in the present release not used, but required in the descriptor file)
* Endianism type (b=big endian or l=little endian)
* List of Ids of the data files (a number representing the order or the time) and names of the data files

Example of Descriptor file: 

.. code-block::

    rawGridsDesc
    density
    3
    Float
    64
    64
    64
    time
    l
    0.0 JET8Xhj.f064.dat.pff
    0.7 JET8Xeh.f064.dat.pff
    2.1 JET8Tat.f064.dat.pff
    
All the files listed in the descriptor file must be given. Each file will be converted and an internal binary table will be created for each listed file.
Output files will have the same name of --out parameter +listedfilename+”.bin” and “.bin.head”. If a filename start with http:// or ftp:// the remote file is downloaded. If the --userpwd option is given the username and password are used for remote access to the file.

Downloaded files are temporarily copied in the same directory given in --out option and cleaned at the end of the import phase. The file :file:`downLoadedFilename_VisIVO_list` in the current directory contains information on the download operations.

Usage:

.. code-block:: console

    $ VisIVOImporter --fformat rawgrids --out /home/user/data/NewTable rawpointsUserFile.desc

VOTable
^^^^^^^
The VOTable format is an XML standard for the interchange of data represented as a set of tables. In this context, a table is an unordered set of rows, each having a uniform format, as specified in the table metadata information. Each row in a table is a sequence of table cells, and each of these contain either a primitive data type or an array of such primitives. It can also contain a link to an external file, that the XML part describes. No VOTables with binary values are supported in VisIVO.

The file sizes that can be processed by the VisIVO Server are only limited by the underlying parsing libraries. As an example, the command below produces :file:`NewTable.bin`, :file:`NewTable.bin.head` from :file:`VOTableUserFileName.xml`. 
This reader has no limit on VOTable size, but can read only ascii data.

Usage:

.. code-block:: console

    $ VisIVOImporter --fformat votable --out /home/user/dataNewTable.bin VOTableUserFilename.xml
    
    
