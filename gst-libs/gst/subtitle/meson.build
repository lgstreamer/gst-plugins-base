gst_subtitle_headers = [
  'subtitle.h',
  'subtitle-prelude.h',
  'gstsubtitlemeta.h'
]
install_headers(gst_subtitle_headers, subdir : 'gstreamer-1.0/gst/subtitle/')

gst_subtitle_sources = ['gstsubtitlemeta.c']

gstsubtitle = library('gstsubtitle-@0@'.format(api_version),
  gst_subtitle_sources,
  c_args : gst_plugins_base_args,
  include_directories: [configinc, libsinc],
  version : libversion,
  soversion : soversion,
  install : true,
  dependencies : gst_dep,
)

subtitle_gen_sources  = []
if build_gir
  gst_gir_extra_args = gir_init_section + [ '--c-include=gst/subtitle/subtitle.h' ]
  subtitle_gen_sources += [gnome.generate_gir(gstsubtitle,
    sources : gst_subtitle_sources + gst_subtitle_headers,
    namespace : 'GstSubtitle',
    nsversion : api_version,
    identifier_prefix : 'Gst',
    symbol_prefix : 'gst',
    export_packages : 'gstreamer-subtitle-1.0',
    includes : ['Gst-1.0'],
    install : true,
    extra_args : gst_gir_extra_args,
    dependencies : gst_dep
  )]
endif

subtitle_dep = declare_dependency(link_with: gstsubtitle,
  include_directories : [libsinc],
  dependencies : gst_dep,
  sources: subtitle_gen_sources)
