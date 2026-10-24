# The ReShade FX compiler (BSD-3-Clause), built as-is without our warning set.
add_library(reshadefx STATIC
    effect_codegen_glsl.cpp
    effect_expression.cpp
    effect_lexer.cpp
    effect_parser_exp.cpp
    effect_parser_stmt.cpp
    effect_preprocessor.cpp
    effect_symbol_table.cpp
)
target_include_directories(reshadefx PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(reshadefx PRIVATE NDEBUG)
set_target_properties(reshadefx PROPERTIES POSITION_INDEPENDENT_CODE ON)
