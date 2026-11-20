#ifndef _TIF_CONFIG_H_
#define _TIF_CONFIG_H_

#include "tiffconf.h"

#define CHUNKY_STRIP_READ_SUPPORT 1
#define DEFER_STRILE_LOAD 1

#define HAVE_ASSERT_H 1
#define HAVE_FCNTL_H 1
#define HAVE_SYS_TYPES_H 1

#ifdef _WIN32
#  define HAVE_IO_H 1
#  define HAVE_SETMODE 1
#  define USE_WIN32_FILEIO 1
#else
#  define HAVE_FSEEKO 1
#  define HAVE_MMAP 1
#  define HAVE_STRINGS_H 1
#  define HAVE_UNISTD_H 1
#  define HAVE_DECL_OPTARG 1
#  define HAVE_GETOPT 1
#endif

#define PACKAGE "tiff"
#define PACKAGE_NAME "LibTIFF Software"
#define PACKAGE_TARNAME "tiff"
#define PACKAGE_VERSION "4.7.0"
#define PACKAGE_STRING "LibTIFF Software 4.7.0"
#define PACKAGE_BUGREPORT "tiff@lists.osgeo.org"
#define PACKAGE_URL ""

#define SIZEOF_SIZE_T 8
#define STRIP_SIZE_DEFAULT 8192
#define TIFF_MAX_DIR_COUNT 1048576

#ifndef WORDS_BIGENDIAN
#  undef WORDS_BIGENDIAN
#endif

#if !defined(__MINGW32__)
#  define TIFF_SIZE_FORMAT "zu"
#  define TIFF_SSIZE_FORMAT PRId64
#else
#  define TIFF_SIZE_FORMAT PRIu64
#  define TIFF_SSIZE_FORMAT PRId64
#endif

#endif /* _TIF_CONFIG_H_ */
