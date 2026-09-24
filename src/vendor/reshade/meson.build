reshade_src = [
    'effect_codegen_spirv.cc',
    'effect_expression.cc',
    'effect_lexer.cc',
    'effect_parser.cc',
    'effect_preprocessor.cc',
    'effect_symbol_table.cc',
]


cpp_args_reshade = cpp.get_supported_arguments(['-Wno-unknown-pragmas'])


reshade_lib = static_library('reshade', reshade_src,
    include_directories : [VKIntox_include_path, include_directories('.')],
    cpp_args : cpp_args_reshade,
    )

reshade_dep = declare_dependency(
    link_with : [ reshade_lib ])
