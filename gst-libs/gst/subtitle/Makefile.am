lib_LTLIBRARIES = libgstsubtitle-@GST_API_VERSION@.la

libgstsubtitle_@GST_API_VERSION@_la_SOURCES = \
	gstsubtitlemeta.c

libgstsubtitle_@GST_API_VERSION@includedir = $(includedir)/gstreamer-@GST_API_VERSION@/gst/subtitle
libgstsubtitle_@GST_API_VERSION@include_HEADERS = \
    subtitle.h \
    subtitle-prelude.h \
    gstsubtitlemeta.h

libgstsubtitle_@GST_API_VERSION@_la_LIBADD = \
  $(GST_BASE_LIBS) $(GST_LIBS)

libgstsubtitle_@GST_API_VERSION@_la_CFLAGS = $(GST_PLUGINS_BASE_CFLAGS) $(GST_BASE_CFLAGS) $(GST_CFLAGS)
libgstsubtitle_@GST_API_VERSION@_la_LDFLAGS = $(GST_LIB_LDFLAGS) $(GST_ALL_LDFLAGS) $(GST_LT_LDFLAGS)

