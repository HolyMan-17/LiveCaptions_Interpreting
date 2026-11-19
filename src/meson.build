livecaptions_sources = [
  'main.c',
  'livecaptions-window.c',
  'livecaptions-welcome.c',
  'livecaptions-settings.c',
  'livecaptions-application.c',
  'audiocap.c',
  'audiocap-pa.c',
  'audiocap-pw.c',
  'asrproc.c',
  'asr-engine.c',
  'asr-engine-april.c',
  'asr-engine-sherpa.c',
  'line-gen.c',
  'transcript.c',
  'profanity-filter.c',
  'window-helper.c',
  'history.c',
  'livecaptions-history-window.c',
  'dbus-interface.c'
]

cc = meson.get_compiler('c')

livecaptions_deps = [
  dependency('libadwaita-1', version: '>= 1.4'),
  #dependency('libpipewire-0.3', version: '>=0.3.41'),
  dependency('libpulse'),
  dependency('x11'),

  cc.find_library('m', required: false),
  # sherpa-onnx is built from C++ sources; the app is C, so libstdc++ must
  # be linked explicitly for the sherpa static libraries to resolve
  cc.find_library('stdc++'),
  april_lib,
  sherpa_lib
]

gnome = import('gnome')

livecaptions_sources += gnome.compile_resources('livecaptions-resources',
  'livecaptions.gresource.xml',
  c_name: 'livecaptions'
)

executable_kwargs = {}
ort_libdir = get_option('onnxruntime_libdir')
if ort_libdir != ''
  executable_kwargs += {'build_rpath': ort_libdir, 'install_rpath': ort_libdir}
endif

executable('livecaptions', livecaptions_sources,
  dependencies: livecaptions_deps,
  install: true,
  kwargs: executable_kwargs,
)
