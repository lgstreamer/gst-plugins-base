playback_sources = [
  'gstdecodebin2.c',
  'gstdecodebin3.c',
  'gsturidecodebin.c',
  'gsturidecodebin3.c',
  'gsturisourcebin.c',
  'gstparsebin.c',
  'gstplayback.c',
  'gstplaybackutils.c',
  'gstplaybin2.c',
  'gstplaybin3.c',
  'gstplaysink.c',
  'gstplay-enum.c',
  'gstsubtitleoverlay.c',
  'gstplaysinkvideoconvert.c',
  'gstplaysinkaudioconvert.c',
  'gstplaysinkconvertbin.c',
  'gststreamsynchronizer.c'
]

#ifdef HAVE_PRIV_FUNC
playback_priv_sources = [
  'gstdecodebin3-parse.c',
  'gstdecodebin3-tv.c',
  'gsturisourcebin-tv.c',
  'gstparsebin-tv.c',
  'gstplaybin3-tv.c',
]
#endif

gstplayback = library('gstplayback',
  playback_sources,
  c_args : gst_plugins_base_args + ['-DCOLORSPACE="videoconvert"'],
  include_directories: [configinc, libsinc],
  dependencies : [audio_dep, video_dep, pbutils_dep, tag_dep],
  install : true,
  install_dir : plugins_install_dir,
)
