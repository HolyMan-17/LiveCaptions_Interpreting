project('livecaptions', 'c',
          version: '0.1.0',
    meson_version: '>= 0.59.0',
  default_options: [ 'warning_level=2',
                     'werror=false',
                     'c_std=gnu11',
                   ],
)

cmake = import('cmake')
april = cmake.subproject('april-asr')

april_lib = april.dependency('aprilasr_static')

# Static sherpa-onnx (with onnxruntime statically linked inside), so the
# process keeps a single dynamic onnxruntime 1.14.1 for april-asr
sherpa_options = cmake.subproject_options()
sherpa_options.add_cmake_defines({
  'BUILD_SHARED_LIBS': 'OFF',
  'SHERPA_ONNX_ENABLE_PYTHON': 'OFF',
  'SHERPA_ONNX_ENABLE_TESTS': 'OFF',
  'SHERPA_ONNX_ENABLE_CHECK': 'OFF',
  'SHERPA_ONNX_ENABLE_PORTAUDIO': 'OFF',
  'SHERPA_ONNX_ENABLE_JNI': 'OFF',
  'SHERPA_ONNX_ENABLE_WEBSOCKET': 'OFF',
  'SHERPA_ONNX_ENABLE_GPU': 'OFF',
  'SHERPA_ONNX_ENABLE_DIRECTML': 'OFF',
  'SHERPA_ONNX_ENABLE_WASM': 'OFF',
  'SHERPA_ONNX_ENABLE_BINARY': 'OFF',
  'SHERPA_ONNX_ENABLE_TTS': 'OFF',
  'SHERPA_ONNX_ENABLE_SPEAKER_DIARIZATION': 'OFF',
  'SHERPA_ONNX_BUILD_C_API_EXAMPLES': 'OFF',
  # Use the pre-installed dynamic onnxruntime 1.14.1 (the one april-asr uses)
  # instead of sherpa's bundled static onnxruntime: the bundled 1.28.2 static
  # lib is built by an older toolchain and its std::regex template
  # instantiations clash with the ones from sherpa's own dependencies built
  # with the system GCC, causing heap corruption inside OrtEnv creation.
  # Environment variables SHERPA_ONNXRUNTIME_INCLUDE_DIR and
  # SHERPA_ONNXRUNTIME_LIB_DIR must point at the onnxruntime 1.14.1 install
  # when configuring.
  'SHERPA_ONNX_USE_PRE_INSTALLED_ONNXRUNTIME_IF_AVAILABLE': 'ON',
  'SHERPA_ONNX_LINK_LIBSTDCPP_STATICALLY': 'ON',
})
sherpa = cmake.subproject('sherpa-onnx', options: sherpa_options)

# Meson's CMake transpiler drops static-library link dependencies between
# targets, so assemble the full chain manually; order matters for static
# linking (dependents before their dependencies)
sherpa_lib = [
  sherpa.get_variable('sherpa_onnx_c_api_dep'),
  sherpa.get_variable('sherpa_onnx_core_dep'),
  sherpa.get_variable('kaldi_decoder_core_dep'),
  sherpa.get_variable('kaldifst_core_dep'),
  sherpa.get_variable('fst_dep'),
  sherpa.get_variable('fstfar_dep'),
  sherpa.get_variable('kaldi_native_fbank_core_dep'),
  sherpa.get_variable('kissfft_dep'),
  sherpa.get_variable('ssentencepiece_core_dep'),
]

i18n = import('i18n')

gnome = import('gnome')

config_h = configuration_data()
config_h.set_quoted('PACKAGE_VERSION', meson.project_version())
config_h.set_quoted('GETTEXT_PACKAGE', 'livecaptions')
config_h.set_quoted('LOCALEDIR', join_paths(get_option('prefix'), get_option('localedir')))
configure_file(
  output: 'livecaptions-config.h',
  configuration: config_h,
)
add_project_arguments([
  '-I' + meson.project_build_root(),
], language: 'c')


subdir('data')
subdir('src')
subdir('po')

gnome.post_install(
  glib_compile_schemas: true,
  gtk_update_icon_cache: true,
  update_desktop_database: false,
)
