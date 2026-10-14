project('VKIntox', ['c', 'cpp'], default_options: ['c_std=c11', 'cpp_std=c++2a'])

fs = import('fs')

# the widgets and the reshade compiler are submodules; an un-initialised clone
# would otherwise fail deep in the include search with no hint why
if not meson.is_subproject() and (not fs.exists('src/vendor/imgui/imgui.h') or
                                  not fs.exists('src/vendor/reshade/effect_parser.hh'))
    error('vendor submodules are missing: run `git submodule update --init --recursive`')
endif
cpp = meson.get_compiler('cpp')

VKIntox_include_path = ''

lib_dir = get_option('libdir')

if get_option('append_libdir_vkintox')
    lib_dir = join_paths(lib_dir, 'vkintox')
endif

if get_option('with_so')
    subdir('src')
endif

if get_option('with_json')
    subdir('packaging/meson-config')
endif

# Everything redistributed here is licensed text that has to travel with a
# system install: the fonts (fetched at install time, but shipped by packagers),
# ImGui and reshade.
install_data(
    'assets/font/LICENSE',
    'assets/font/LICENSE-Apache-2.0.txt',
    'assets/font/LICENSE-SIL-OFL-1.1.txt',
    'assets/font/LICENSE-GoogleSansFlex-OFL.txt',
    'src/vendor/reshade/LICENSE.md',
    'src/vendor/imgui/LICENSE.txt',
    install_dir : get_option('datadir') / 'licenses' / 'vkintox',
)
