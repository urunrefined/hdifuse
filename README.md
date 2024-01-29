# FAT12 HDI file mounting / inspecting

The binaries compiled by this project will allow you to mount and examine
the first FAT12 Volume in a HDI file. HDI files are basically images,
which most likely contain a FAT12 volume somewhere inside of them.

Using these tools these volumes can be mounted, information about the HDI file printed,
and files deleted, modified and added, the additions being written to the
volume once the image is properly umounted. Please check the section "Limitations"
first, as there are a few.

## Building:
*make* will build the project and create all executables.
You will need libfuse3 installed to both build and run the hdifuse program.

## Running

### hdifuse
Mounts the first FAT12 Volume in specified image to mountpoint
Execute via *./hdifuse 'HDIFILE' 'MOUNTPOINT'*

### hdiimgmanip
Prints the HDI header information of specified image
Execute via *./hdiimgmanip 'HDIFILE'*

If a second argument is given, the header of the image will be removed
and the remaining image written to the path, which is indiacated by
the second argument
Execute via *./hdiimgmanip 'HDIFILE' 'HDIFILEWITHOUTHEADER'*

### hdiprint
Prints various information about the first FAT12 volume
contained in the HDI image.
Execute via *./hdiprint 'HDIFILE'*

## hdifuse
hdifuse will scan each 512 bytes (which is the lowest sector size) and
determine if there is code which suggest there is a FAT12 volume present.
If there is an hit, it will try and mount the found volume. If this does not
succeed it will keep scanning until ther next hit. If there is no volume present
the program will exit.

Execute via *./hdifuse 'FILETOMOUNT' 'MOUNTPOINT'*

For debug output use the -d flag, i.e. *./hdifuse -d ...*

## hdifdisk
hdifdisk will do a non-exhaustive check on the first FAT12 volume in the given file
and will print various information.

You may call hdifdisk via *./hdifdisk 'HDIFILE'*

For showing information on all clusters in the first FAT
use *./hdifdisk -l 'HDIFILE'* or specify the FAT indices after *-l*
to show a selection

hdifdisk will show found orphans, which are indices in the FAT, which are not
referenced in any of the files on the file-system. This may or may not be an actual
error.

To modify an FAT12 index of the first FAT use *./hdifdisk -m 'index' -s 'value' 'HDIFILE'*
Make sure to backup the image first.

Example, to free orphans 33 and 1045 use ./hdifdisk -m 33 1045 -s 0 'HDIFILE'

Please note afterwards this action will sync all FATs in the FAT region with the first
FAT.

## Limitations
Please note the following limitations:

* The codepage is assumed to be MS932. _ANY OTHER CODEPAGE IN A FAT12 VOLUME CONTAINED IN A HDI FILE WILL GIVE YOU GARBAGE_. If the volume filenames are encoded in JIS X 0201, this should also work, as MS932 is compatible with this
* There is no support for directory creation / deletion (I simply havent had any reason to implement this yet)
* There is no support for "forgetting" files which have a positive lookup counter. Files will however be forgotten instantly when unlink -> forget is called. This is because I simply have no way of testing this properly. I have so much ram the kernel never ever frees any cached inodes. There is also no way (as far as I know anyway) to force the kernel to mount an FS in a more aggressive forgetting mode
* There is no support for extending IO.SYS/MSDOS.SYS. DO NOT MODIFY IO.SYS or MSDOS.SYS if these changes cause clusters to be allocated for these files, which would lead to the cluster-chain for these files to not be contigious
* Read only file attributes are ignored
* Hidden file attributes are ignored
* Any string given of an operation done on this specific fusefs layer is assumed to be utf8. Anything else will probably not work
* There is no way of adding directories, only files
* There is no way to set MS-DOS specific attributes on files
* Directory clusters which do not contain any valid files are not truncated
* The only end of chain cluster considered is 0xFFF
* There are no checks for < clusters less or equal to cluster 2 when stepping through cluster chains and accessing the data region
* There are no checks for checking the fat clusters are in range of the data region
* There are probably lots of bugs
