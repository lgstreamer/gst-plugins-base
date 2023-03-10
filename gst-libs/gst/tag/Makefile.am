libgsttagincludedir = \
	$(includedir)/gstreamer-@GST_API_VERSION@/gst/tag

glib_enum_define = GST_TAG
glib_gen_prefix = gst_tag
glib_gen_basename = tag
glib_gen_decl_banner=GST_TAG_API
glib_gen_decl_include=\#include <gst/tag/tag-prelude.h>

glib_enum_headers = tag.h gsttagdemux.h

built_sources = tag-enumtypes.c
built_headers = tag-enumtypes.h
BUILT_SOURCES = $(built_sources) $(built_headers)

include $(top_srcdir)/common/gst-glib-gen.mak

libgsttaginclude_HEADERS = \
	tag.h tag-prelude.h gsttagdemux.h gsttagmux.h xmpwriter.h
nodist_libgsttaginclude_HEADERS = tag-enumtypes.h

CLEANFILES = $(BUILT_SOURCES)

lib_LTLIBRARIES = libgsttag-@GST_API_VERSION@.la

libgsttag_@GST_API_VERSION@_la_SOURCES = \
	gstvorbistag.c gstid3tag.c gstxmptag.c gstexiftag.c \
	lang.c licenses.c tags.c gsttagdemux.c gsttagmux.c \
	gsttageditingprivate.c id3v2.c id3v2frames.c xmpwriter.c
nodist_libgsttag_@GST_API_VERSION@_la_SOURCES = $(BUILT_SOURCES)

libgsttag_@GST_API_VERSION@_la_CFLAGS = $(GST_PLUGINS_BASE_CFLAGS) \
	$(GST_BASE_CFLAGS) $(GST_CFLAGS) $(ZLIB_CFLAGS) \
	-DLICENSE_TRANSLATIONS_PATH=\"$(pkgdatadir)/@GST_API_VERSION@/license-translations.dict\"
libgsttag_@GST_API_VERSION@_la_LIBADD = $(GST_BASE_LIBS) $(GST_LIBS) $(LIBM) $(ZLIB_LIBS)
libgsttag_@GST_API_VERSION@_la_LDFLAGS = $(GST_LIB_LDFLAGS) $(GST_ALL_LDFLAGS) $(GST_LT_LDFLAGS)

# lang-tables.dat contains generated static data and is included by lang.c
# licenses-tables.dat contains generated data and is included by licenses.c
EXTRA_DIST = lang-tables.dat licenses-tables.dat license-translations.dict

noinst_HEADERS = gsttageditingprivate.h id3v2.h

if HAVE_INTROSPECTION
BUILT_GIRSOURCES = GstTag-@GST_API_VERSION@.gir

gir_headers=$(patsubst %,$(srcdir)/%, $(libgsttaginclude_HEADERS))
gir_headers+=$(patsubst %,$(builddir)/%, $(nodist_libgsttaginclude_HEADERS))
gir_sources=$(patsubst %,$(srcdir)/%, $(libgsttag_@GST_API_VERSION@_la_SOURCES))
gir_sources+=$(patsubst %,$(builddir)/%, $(nodist_libgsttag_@GST_API_VERSION@_la_SOURCES))

GstTag-@GST_API_VERSION@.gir: $(INTROSPECTION_SCANNER) libgsttag-@GST_API_VERSION@.la
	$(AM_V_GEN)PKG_CONFIG_PATH="$(GST_PKG_CONFIG_PATH)" GI_SCANNER_DISABLE_CACHE=yes\
		GST_PLUGIN_SYSTEM_PATH_1_0="" GST_PLUGIN_PATH_1_0="" GST_REGISTRY_DISABLE=yes \
		CPPFLAGS="$(CPPFLAGS)" CFLAGS="$(CFLAGS)" LDFLAGS="$(LDFLAGS)" CC="$(CC)" PKG_CONFIG="$(PKG_CONFIG)" DLLTOOL="$(DLLTOOL)" \
		$(INTROSPECTION_SCANNER) -v --namespace GstTag \
		--nsversion=@GST_API_VERSION@ \
		--identifier-prefix=Gst \
		--symbol-prefix=gst \
		--warn-all \
		--c-include "gst/tag/tag.h" \
		-I$(top_srcdir)/gst-libs \
		-I$(top_builddir)/gst-libs \
		--add-include-path=$(PKG_CONFIG_SYSROOT_DIR)`PKG_CONFIG_PATH="$(GST_PKG_CONFIG_PATH)" $(PKG_CONFIG) --variable=girdir gstreamer-@GST_API_VERSION@` \
		--add-include-path=$(PKG_CONFIG_SYSROOT_DIR)`PKG_CONFIG_PATH="$(GST_PKG_CONFIG_PATH)" $(PKG_CONFIG) --variable=girdir gstreamer-base-@GST_API_VERSION@` \
		--library=libgsttag-@GST_API_VERSION@.la \
		--include=Gst-@GST_API_VERSION@ \
		--include=GstBase-@GST_API_VERSION@ \
		--libtool="$(top_builddir)/libtool" \
		--pkg gstreamer-@GST_API_VERSION@ \
		--pkg gstreamer-base-@GST_API_VERSION@ \
		--pkg-export gstreamer-tag-@GST_API_VERSION@ \
		--add-init-section="$(INTROSPECTION_INIT)" \
		--output $@ \
		$(gir_headers) \
		$(gir_sources)

# INTROSPECTION_GIRDIR/INTROSPECTION_TYPELIBDIR aren't the right place to
# install anything - we need to install inside our prefix.
girdir = $(datadir)/gir-1.0
gir_DATA = $(BUILT_GIRSOURCES)

typelibsdir = $(libdir)/girepository-1.0/

typelibs_DATA = $(BUILT_GIRSOURCES:.gir=.typelib)

%.typelib: %.gir $(INTROSPECTION_COMPILER)
	$(AM_V_GEN)PKG_CONFIG_PATH="$(GST_PKG_CONFIG_PATH)" \
		$(INTROSPECTION_COMPILER) \
		--includedir=$(srcdir) \
		--includedir=$(builddir) \
		--includedir=$(PKG_CONFIG_SYSROOT_DIR)`PKG_CONFIG_PATH="$(GST_PKG_CONFIG_PATH)" $(PKG_CONFIG) --variable=girdir gstreamer-@GST_API_VERSION@` \
		--includedir=$(PKG_CONFIG_SYSROOT_DIR)`PKG_CONFIG_PATH="$(GST_PKG_CONFIG_PATH)" $(PKG_CONFIG) --variable=girdir gstreamer-base-@GST_API_VERSION@` \
		$(INTROSPECTION_COMPILER_OPTS) $< -o $(@F)

CLEANFILES += $(BUILT_GIRSOURCES) $(typelibs_DATA)
endif

# little program that reads iso_639.xml and outputs tables for us as fallback
# for when iso-codes are not available (and so we don't have to read the xml
# just to map codes)
mklangtables_SOURCES = mklangtables.c
mklangtables_CFLAGS = $(GST_CFLAGS)
mklangtables_LDADD = $(GST_LIBS)

mklicensestables_SOURCES = mklicensestables.c
mklicensestables_CFLAGS = $(GST_CFLAGS)
mklicensestables_LDADD = $(GST_LIBS)

EXTRA_PROGRAMS = mklangtables mklicensestables

update-isocodes: mklangtables
	$(builddir)/mklangtables > $(srcdir)/lang-tables.dat && \
		echo "Updated lang-tables.dat"

update-licenses: mklicensestables
	$(builddir)/mklicensestables \
		--translation-dictionary=$(srcdir)/license-translations.dict \
		> $(srcdir)/licenses-tables.dat && \
		echo "Updated licenses-tables.dat and license-translations.dict"

#if USE_NLS
# Yes, this is not great, but it's only an implementation detail. The
# translations come from an external source here, so we don't want the
# strings retranslated, but also we don't want to create 30 1kB .mo files,
# so just do something for now, we can change it later if someone really cares.
licensetransdir = $(pkgdatadir)/@GST_API_VERSION@
licensetrans_DATA = $(srcdir)/license-translations.dict
#endif
