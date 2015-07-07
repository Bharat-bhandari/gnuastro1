# Crop an image based polygon from an image.
#
# See the Tests subsection of the manual for a complete explanation
# (in the Installing gnuastro section).
#
# Original author:
#     Mohammad Akhlaghi <akhlaghi@gnu.org>
# Contributing author(s):
#
# Copying and distribution of this file, with or without modification,
# are permitted in any medium without royalty provided the copyright
# notice and this notice are preserved.  This file is offered as-is,
# without any warranty.





# Preliminaries:
################
# Set the variabels (The executable is in the build tree). Do the
# basic checks to see if the executable is made or if the defaults
# file exists (basicchecks.sh is in the source tree).
prog=imgcrop
execname=../src/$prog/ast$prog





# If the executable was not made (the user chose to not install this
# package), skip this test:
if [ ! -f $execname ]; then
    exit 77
fi





# Actual test script:
#####################

# The number of threads is one so if CFITSIO does is not configured to
# enable multithreaded access to files, the tests pass. It is the
# users choice to enable this feature.

img=mkprofcat1.fits
$execname $img $cat --imgmode --polygon=209,50:436.76,151:475.64,438.2:210.6,454.04:121.4,289.88 --zeroisnotblank
mv mkprofcat1_crop.fits imgpolygon.fits