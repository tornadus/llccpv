project('llccpv', 'c', 'cpp',
  version : '0.1.0',
  default_options : ['c_std=c17', 'cpp_std=c++17', 'warning_level=2', 'optimization=2'])

sdl3_dep = dependency('sdl3')
gl_dep = dependency('gl')
threads_dep = dependency('threads')
pipewire_dep = dependency('libpipewire-0.3')
qt6_dep = dependency('qt6', modules : ['Core', 'Widgets'])

install_data(
  'shaders/quad.vert',
  'shaders/yuyv.frag',
  'shaders/uyvy.frag',
  'shaders/nv12.frag',
  'shaders/passthrough.frag',
  'shaders/sharp_bilinear.frag',
  install_dir : get_option('datadir') / 'llccpv' / 'shaders',
)

src = files(
  'src/main.c',
  'src/capture.c',
  'src/render.c',
  'src/shader.c',
  'src/audio.c',
  'src/picker.cpp',
)

executable('llccpv', src,
  dependencies : [sdl3_dep, gl_dep, pipewire_dep, threads_dep, qt6_dep],
  install : true)
