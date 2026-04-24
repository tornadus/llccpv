project('llccpv', 'c', 'cpp',
  version : '0.1.0',
  default_options : ['c_std=c17', 'cpp_std=c++17', 'warning_level=2', 'optimization=2'])

sdl3_dep = dependency('sdl3')
gl_dep = dependency('gl')
threads_dep = dependency('threads')
pipewire_dep = dependency('libpipewire-0.3')
qt6_dep = dependency('qt6', modules : ['Core', 'Widgets'])
cc = meson.get_compiler('c')
m_dep = cc.find_library('m', required : false)

install_data(
  'shaders/quad.vert',
  'shaders/yuyv.frag',
  'shaders/uyvy.frag',
  'shaders/nv12.frag',
  'shaders/passthrough.frag',
  'shaders/sharp_bilinear.frag',
  'shaders/fsr_easu.frag',
  'shaders/fsr_rcas.frag',
  install_dir : get_option('datadir') / 'llccpv' / 'shaders',
)

install_data(
  'shaders/ffx/ffx_a.h',
  'shaders/ffx/ffx_fsr1.h',
  install_dir : get_option('datadir') / 'llccpv' / 'shaders' / 'ffx',
)

# Core render + shader code shared by the app and tests.
render_core = static_library('render_core',
  ['src/render.c', 'src/shader.c'],
  dependencies : [gl_dep, m_dep],
)

llccpv_exe = executable('llccpv',
  ['src/main.c', 'src/capture.c', 'src/capture_mailbox.c',
   'src/audio.c', 'src/ring.c', 'src/picker.cpp'],
  link_with : render_core,
  dependencies : [sdl3_dep, gl_dep, pipewire_dep, threads_dep, qt6_dep],
  install : true)

# --- Tests ---
src_inc = include_directories('src')

test_support = files(
  'tests/support/gl_harness.c',
  'tests/support/ref_yuv.c',
  'tests/support/ref_scale.c',
  'tests/support/fixtures.c',
)

test_pixel = executable('test_pixel',
  ['tests/test_pixel.c', test_support],
  include_directories : src_inc,
  link_with : render_core,
  dependencies : [sdl3_dep, gl_dep, m_dep],
  build_by_default : false,
)
test('pixel', test_pixel, workdir : meson.project_source_root())

test_audio = executable('test_audio',
  ['tests/test_audio.c', 'src/ring.c'],
  include_directories : src_inc,
  dependencies : [threads_dep],
  build_by_default : false,
)
test('audio', test_audio, timeout : 10)

test_capture_mailbox = executable('test_capture_mailbox',
  ['tests/test_capture_mailbox.c', 'src/capture_mailbox.c'],
  include_directories : src_inc,
  dependencies : [threads_dep],
  build_by_default : false,
)
test('capture_mailbox', test_capture_mailbox, timeout : 10)

# --- e2e helpers ---
feeder_exe = executable('feeder',
  'tests/e2e/feeder/feeder.c',
  build_by_default : false,
)

# Meson-integrated e2e runner. TAP protocol so individual test results
# surface, and the wrapper emits a SKIP directive when the llccpv-test
# loopback device isn't present — `meson test` always completes without
# failing the run. Timeout covers the ~70 s quick-run suite; stress tests
# remain opt-in via tests/e2e/run.sh --stress.
test('e2e',
  find_program('tests/e2e/meson_test.sh'),
  protocol : 'tap',
  workdir  : meson.project_source_root(),
  timeout  : 180,
  depends  : [llccpv_exe, feeder_exe],
)
