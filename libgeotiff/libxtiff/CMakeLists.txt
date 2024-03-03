###############################################################################
#
# CMake configuration file to build libxtiff library
#
# Author: Mateusz Loskot <mateusz@loskot.net>
#
###############################################################################

ADD_LIBRARY(xtiff STATIC xtiff.c)
TARGET_LINK_LIBRARIES(xtiff
    ${TIFF_TARGET})
