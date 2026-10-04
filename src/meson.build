subdir('vendor/reshade')

glsl_shader_src = files(
    'effects/shaders/glsl/depth_resolve.frag.glsl',
    'effects/shaders/glsl/depth_resolve_ms.frag.glsl',
    'effects/shaders/glsl/depth_resolve_msaa.frag.glsl',
    'effects/shaders/glsl/depth_resolve_alpha.frag.glsl',
    'effects/shaders/glsl/depth_resolve_alpha_ms.frag.glsl',
    'effects/shaders/glsl/depth_resolve_alpha_msaa.frag.glsl',
    'effects/shaders/glsl/depth_resolve_inverted.frag.glsl',
    'effects/shaders/glsl/depth_resolve_inverted_ms.frag.glsl',
    'effects/shaders/glsl/depth_resolve_inverted_msaa.frag.glsl',
    'effects/shaders/glsl/depth_resolve_universal.frag.glsl',
    'effects/shaders/glsl/depth_resolve_universal_ms.frag.glsl',
    'effects/shaders/glsl/depth_resolve_universal_msaa.frag.glsl',
    'effects/shaders/glsl/full_screen_triangle.vert.glsl',
)
glsl_compiler = find_program('glslangValidator')
glsl_generator = generator(glsl_compiler,
    output: ['@BASENAME@.h'],
    arguments: ['-V', '-x', '@INPUT@', '-o', '@OUTPUT@'])
shader_include = glsl_generator.process(glsl_shader_src)

imgui_src = [
    'vendor/imgui/imgui.cc',
    'vendor/imgui/imgui_draw.cc',
    'vendor/imgui/imgui_tables.cc',
    'vendor/imgui/imgui_widgets.cc',
    'vendor/imgui/imgui_m3.cc',
    'vendor/imgui/backends/imgui_impl_vulkan.cc',
]

overlay_src = [
    'overlay/imgui_overlay.cc',
    'overlay/view_add_effects.cc',
    'overlay/view_advanced.cc',
    'overlay/view_configs.cc',
    'overlay/view_debug.cc',
    'overlay/view_diagnostics.cc',
    'overlay/view_about.cc',
    'overlay/view_settings.cc',
    'overlay/view_shader_manager.cc',
    'overlay/view_shader_test.cc',
    'overlay/view_main.cc',
    'overlay/params/field_editor.cc',
    'overlay/params/fields/field_bool.cc',
    'overlay/params/fields/field_float.cc',
    'overlay/params/fields/field_floatvec.cc',
    'overlay/params/fields/field_int.cc',
    'overlay/params/fields/field_intvec.cc',
    'overlay/params/fields/field_uint.cc',
    'overlay/params/fields/field_uintvec.cc',
]

VKIntox_src = [
    'core/vkintox.cc',
    'core/vkintox_depth.cc',
    'render/buffer.cc',
    'render/command_buffer.cc',
    'render/vk_handle.cc',
    'core/config.cc',
    'core/config_serializer.cc',
    'core/config_serializer_profile.cc',
    'core/settings_manager.cc',
    'render/descriptor_set.cc',
    'effects/effect.cc',
    'effects/effect_registry.cc',
    'effects/effect_reshade.cc',
    'effects/effect_transfer.cc',
    'effects/reshade_parser.cc',
    'render/fake_swapchain.cc',
    'render/format.cc',
    'render/framebuffer.cc',
    'render/graphics_pipeline.cc',
    'render/image.cc',
    'render/image_view.cc',
    'input/keyboard_input.cc',
    'core/logger.cc',
    'render/logical_swapchain.cc',
    'render/memory.cc',
    'render/renderpass.cc',
    'effects/reshade_uniforms.cc',
    'render/sampler.cc',
    'render/shader.cc',
    'vendor/stb/stb_image.c',
    'vendor/stb/stb_image_resize.c',
    'core/util.cc',
    'render/vkdispatch.cc',
]

x11_dep = dependency('x11')
xi_dep = dependency('xi')
wayland_client_dep = dependency('wayland-client')
xkbcommon_dep = dependency('xkbcommon')
wayland_protocols_dep = dependency('wayland-protocols')
wayland_scanner = find_program('wayland-scanner')

# Generate pointer-constraints-unstable-v1 protocol code
wl_protocols_dir = wayland_protocols_dep.get_variable('pkgdatadir')
pointer_constraints_xml = join_paths(wl_protocols_dir, 'unstable', 'pointer-constraints', 'pointer-constraints-unstable-v1.xml')

pointer_constraints_client_h = custom_target('pointer-constraints-client-header',
    input: pointer_constraints_xml,
    output: 'pointer-constraints-unstable-v1-client-protocol.h',
    command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])

pointer_constraints_code = custom_target('pointer-constraints-code',
    input: pointer_constraints_xml,
    output: 'pointer-constraints-unstable-v1-protocol.c',
    command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])

conf_paths = configuration_data()
conf_paths.set_quoted('SYSCONFDIR', join_paths(get_option('prefix'), get_option('sysconfdir')))
conf_paths.set_quoted('DATADIR', join_paths(get_option('prefix'), get_option('datadir')))

configure_file(output: 'config_paths.hh',
               configuration : conf_paths)

# the binary reports its own version, so it must not depend on its filename or
# on the setup script dropping a version file next to the config.
fs = import('fs')
version_string = 'unknown'
if fs.exists('../VERSION')
    version_string = fs.read('../VERSION').strip()
    if version_string == ''
        version_string = 'unknown'
    endif
endif

conf_version = configuration_data()
conf_version.set_quoted('VKINTOX_VERSION', version_string)

configure_file(output: 'version.hh',
               configuration : conf_version)

# X11/X.h library contains many definitions (e.g. None)
# which conflicts with the rest codebase with with Meson --unity=on option
keyboard_input_x11_lib = static_library('keyboard_input_x11', 'input/keyboard_input_x11.cc', include_directories: [include_directories('core')])
mouse_input_lib = static_library('mouse_input', 'input/mouse_input.cc', include_directories: [include_directories('core')])
input_blocker_lib = static_library('input_blocker', 'input/input_blocker.cc', include_directories: [include_directories('core')])

# X11 input backends. The shared connection (x11_display) owns the single event
# queue; keyboard and mouse register callbacks into it and the blocker grabs on
# the same connection. Kept out of the unity-compiled core because X11/X.h
# defines symbols (None, ...) that collide with the codebase.
x11_display_lib = static_library('x11_display',
    'input/x11_display.cc',
    include_directories: [include_directories('core')])
mouse_input_x11_lib = static_library('mouse_input_x11',
    'input/mouse_input_x11.cc',
    include_directories: [include_directories('core')])

# Wayland input backends — built as static libs to isolate wayland-client headers.
# wayland_input_common provides a shared seat/queue used by both keyboard and mouse.
wayland_input_common_lib = static_library('wayland_input_common',
    'input/wayland_input_common.cc', 'input/wayland_display.cc',
    dependencies: [wayland_client_dep],
    include_directories: [include_directories('core')])
keyboard_input_wayland_lib = static_library('keyboard_input_wayland',
    'input/keyboard_input_wayland.cc',
    link_with: [wayland_input_common_lib],
    dependencies: [wayland_client_dep, xkbcommon_dep],
    include_directories: [include_directories('core')])
mouse_input_wayland_lib = static_library('mouse_input_wayland',
    'input/mouse_input_wayland.cc',
    link_with: [wayland_input_common_lib],
    dependencies: [wayland_client_dep],
    include_directories: [include_directories('core')])
wayland_pointer_constraints_lib = static_library('wayland_pointer_constraints',
    'input/wayland_pointer_constraints.cc', pointer_constraints_code,
    pointer_constraints_client_h,
    link_with: [wayland_input_common_lib],
    dependencies: [wayland_client_dep],
    include_directories: [include_directories('core')])
# Wayland event interposition — wraps game's pointer/keyboard listeners to
# suppress events when overlay has input blocked. Needs default visibility
# for the wl_proxy_add_listener symbol override.
wayland_interpose_lib = static_library('wayland_interpose',
    'input/wayland_interpose.cc',
    dependencies: [wayland_client_dep],
    c_args: ['-fvisibility=default'],
    cpp_args: ['-fvisibility=default'],
    include_directories: [include_directories('core')])

imgui_inc = include_directories('vendor/imgui')
overlay_inc = include_directories('overlay')
effects_inc = include_directories('effects')
effects_params_inc = include_directories('effects/params')
core_inc = include_directories('core')
input_inc = include_directories('input')
render_inc = include_directories('render')
vendor_inc = include_directories('vendor')
source_inc = include_directories('.', 'core', 'input', 'render', 'effects', 'overlay')
stb_inc = include_directories('vendor/stb')
shader_asset_inc = include_directories('effects/shaders')

# The XDG desktop portal is reached over the session bus through GIO, which is
# present on the desktop and inside Flatpak runtimes. Optional: without it the
# shader manager falls back to the in-overlay file browser.
gio_dep = dependency('gio-2.0', required: false)
vkintox_deps = [x11_dep, xi_dep, wayland_client_dep, xkbcommon_dep, reshade_dep]
vkintox_cpp_args = ['-DIMGUI_IMPL_VULKAN_NO_PROTOTYPES']
if gio_dep.found()
    vkintox_deps += gio_dep
    vkintox_cpp_args += '-DVKINTOX_HAVE_GIO=1'
endif

shared_library('vkintox',
    VKIntox_src, overlay_src, imgui_src, shader_include,
    link_with: [keyboard_input_x11_lib, mouse_input_lib, input_blocker_lib,
                x11_display_lib, mouse_input_x11_lib,
                wayland_input_common_lib, keyboard_input_wayland_lib, mouse_input_wayland_lib,
                wayland_pointer_constraints_lib, wayland_interpose_lib],
    include_directories : [VKIntox_include_path, imgui_inc, overlay_inc, effects_inc, effects_params_inc, core_inc, input_inc, render_inc, vendor_inc, include_directories('vendor/reshade'), stb_inc, shader_asset_inc, source_inc],
    cpp_args : vkintox_cpp_args,
    dependencies : vkintox_deps,
    gnu_symbol_visibility: 'hidden',
    install : true,
    install_dir : lib_dir)

executable('vkintox-shader-check',
    'effects/shader_check.cc',
    'effects/reshade_parser.cc',
    'vendor/reshade/effect_codegen_spirv.cc',
    'vendor/reshade/effect_expression.cc',
    'vendor/reshade/effect_lexer.cc',
    'vendor/reshade/effect_parser.cc',
    'vendor/reshade/effect_preprocessor.cc',
    'vendor/reshade/effect_symbol_table.cc',
    'render/vk_handle.cc',
    'core/config.cc',
    'core/config_serializer.cc',
    'core/config_serializer_profile.cc',
    'core/logger.cc',
    'core/util.cc',
    include_directories : [VKIntox_include_path, imgui_inc, overlay_inc, effects_inc, effects_params_inc, core_inc, input_inc, render_inc, vendor_inc, include_directories('vendor/reshade'), stb_inc, source_inc],
    dependencies : [reshade_dep],
    install : true)

config_serializer_tests = executable('vkintox-config-serializer-tests',
    'tests/config_serializer_test.cc',
    'core/config_serializer.cc',
    'core/config_serializer_profile.cc',
    'core/config.cc',
    'core/logger.cc',
    'core/util.cc',
    include_directories : [core_inc, render_inc, effects_inc, effects_params_inc, vendor_inc, source_inc])
test('config serializer', config_serializer_tests)

# The M3 theme is the one part of the UI that cannot be eyeballed in CI: it needs
# no GPU, but a colour-maths or file-format regression is otherwise invisible.
# This links the theme plus the ImGui core (no renderer) and exercises it headless.
m3_theme_tests = executable('vkintox-m3-theme-tests',
    'tests/m3_theme_test.cc',
    'vendor/imgui/imgui.cc',
    'vendor/imgui/imgui_draw.cc',
    'vendor/imgui/imgui_tables.cc',
    'vendor/imgui/imgui_widgets.cc',
    'vendor/imgui/imgui_m3.cc',
    include_directories : [imgui_inc])
test('m3 theme', m3_theme_tests)

# The deferred destroy queue's release order is load-bearing for driver
# stability and impossible to observe without a real GPU, so it is tested
# directly rather than only through the smoke test.
vk_handle_tests = executable('vkintox-vk-handle-tests',
    'render/vk_handle_test.cc',
    'render/vk_handle.cc',
    'core/logger.cc',
    include_directories : [core_inc, render_inc, source_inc])
test('vk handle ordering', vk_handle_tests)

# DepthCopyState serializes the CmdEndRenderPass* -> QueueSubmit handoff and the
# copy ring against DestroyDevice. A missing lock only shows as an intermittent
# device loss under load, so the state is hammered directly here.
threads_dep = dependency('threads')
depth_copy_state_tests = executable('vkintox-depth-copy-state-tests',
    'render/depth_copy_state_test.cc',
    dependencies : [threads_dep],
    include_directories : [render_inc, source_inc])
test('depth copy state', depth_copy_state_tests)

vkintox_run = configure_file(
    input: '../scripts/vkintox-run.in',
    output: 'vkintox-run',
    copy: true,
)

install_data(
    vkintox_run,
    install_dir: get_option('bindir'),
    install_mode: ['rwxr-xr-x'],
)
