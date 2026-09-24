subdir('vendor/reshade')

glsl_shader_src = files(
    'effects/shaders/glsl/cas.frag.glsl',
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
    'effects/shaders/glsl/deband.frag.glsl',
    'effects/shaders/glsl/dls.frag.glsl',
    'effects/shaders/glsl/full_screen_triangle.vert.glsl',
    'effects/shaders/glsl/fxaa.frag.glsl',
    'effects/shaders/glsl/lut.frag.glsl',
    'effects/shaders/glsl/smaa_blend.frag.glsl',
    'effects/shaders/glsl/smaa_blend.vert.glsl',
    'effects/shaders/glsl/smaa_edge_color.frag.glsl',
    'effects/shaders/glsl/smaa_edge_luma.frag.glsl',
    'effects/shaders/glsl/smaa_edge.vert.glsl',
    'effects/shaders/glsl/smaa_neighbor.frag.glsl',
    'effects/shaders/glsl/smaa_neighbor.vert.glsl',
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
    'vendor/imgui/backends/imgui_impl_vulkan.cc',
]

overlay_src = [
    'overlay/imgui_overlay.cc',
    'overlay/view_add_effects.cc',
    'overlay/view_advanced.cc',
    'overlay/view_configs.cc',
    'overlay/view_debug.cc',
    'overlay/view_diagnostics.cc',
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
    'render/buffer.cc',
    'render/command_buffer.cc',
    'core/config.cc',
    'core/config_serializer.cc',
    'core/settings_manager.cc',
    'render/descriptor_set.cc',
    'effects/effect.cc',
    'effects/effect_registry.cc',
    'effects/effect_reshade.cc',
    'effects/effect_simple.cc',
    'effects/effect_transfer.cc',
    'effects/builtin/builtin_effects.cc',
    'effects/builtin/effect_cas.cc',
    'effects/builtin/effect_deband.cc',
    'effects/builtin/effect_dls.cc',
    'effects/builtin/effect_fxaa.cc',
    'effects/builtin/effect_lut.cc',
    'effects/builtin/effect_smaa.cc',
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
    'render/lut_cube.cc',
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

# X11/X.h library contains many definitions (e.g. None)
# which conflicts with the rest codebase with with Meson --unity=on option
keyboard_input_x11_lib = static_library('keyboard_input_x11', 'input/keyboard_input_x11.cc', include_directories: [include_directories('core')])
mouse_input_lib = static_library('mouse_input', 'input/mouse_input.cc', include_directories: [include_directories('core')])
input_blocker_lib = static_library('input_blocker', 'input/input_blocker.cc', include_directories: [include_directories('core')])

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
effects_builtin_inc = include_directories('effects/builtin')
effects_params_inc = include_directories('effects/params')
core_inc = include_directories('core')
input_inc = include_directories('input')
render_inc = include_directories('render')
vendor_inc = include_directories('vendor')
source_inc = include_directories('.', 'core', 'input', 'render', 'effects', 'overlay')
stb_inc = include_directories('vendor/stb')
shader_asset_inc = include_directories('effects/shaders')

shared_library('vkintox',
    VKIntox_src, overlay_src, imgui_src, shader_include,
    link_with: [keyboard_input_x11_lib, mouse_input_lib, input_blocker_lib,
                wayland_input_common_lib, keyboard_input_wayland_lib, mouse_input_wayland_lib,
                wayland_pointer_constraints_lib, wayland_interpose_lib],
    include_directories : [VKIntox_include_path, imgui_inc, overlay_inc, effects_inc, effects_builtin_inc, effects_params_inc, core_inc, input_inc, render_inc, vendor_inc, include_directories('vendor/reshade'), stb_inc, shader_asset_inc, source_inc],
    cpp_args : ['-DIMGUI_IMPL_VULKAN_NO_PROTOTYPES'],
    dependencies : [x11_dep, xi_dep, wayland_client_dep, xkbcommon_dep, reshade_dep],
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
    'core/config.cc',
    'core/config_serializer.cc',
    'core/logger.cc',
    include_directories : [VKIntox_include_path, imgui_inc, overlay_inc, effects_inc, effects_builtin_inc, effects_params_inc, core_inc, input_inc, render_inc, vendor_inc, include_directories('vendor/reshade'), stb_inc, source_inc],
    dependencies : [reshade_dep],
    install : true)

# LD_AUDIT library for Wine Wayland input interposition.
# Redirects wl_proxy_add_listener bindings from dlopen(RTLD_LOCAL) scopes
# (e.g. Wine's winewayland.so) to our wrapper in libvkintox.so.
dl_dep = cpp.find_library('dl')
shared_library('vkintox-audit',
    'input/wayland_audit.c',
    dependencies: [dl_dep],
    install : true,
    install_dir : lib_dir)

vkintox_run_conf = configuration_data()
vkintox_run_conf.set_quoted('VKINTOX_LIB', join_paths(get_option('prefix'), lib_dir, 'libvkintox.so'))
vkintox_run_conf.set_quoted('VKINTOX_AUDIT_LIB', join_paths(get_option('prefix'), lib_dir, 'libvkintox-audit.so'))

vkintox_run = configure_file(
    input: '../scripts/vkintox-run.in',
    output: 'vkintox-run',
    configuration: vkintox_run_conf,
)

install_data(
    vkintox_run,
    install_dir: get_option('bindir'),
    install_mode: ['rwxr-xr-x'],
)
