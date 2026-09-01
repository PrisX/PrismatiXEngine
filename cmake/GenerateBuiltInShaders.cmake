foreach(required IN ITEMS INPUT_SPIRV INPUT_DXIL INPUT_MSL OUTPUT)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "GenerateBuiltInShaders.cmake requires ${required}")
    endif()
endforeach()

function(prismatix_binary_array input name output_variable)
    file(READ "${input}" bytes HEX)
    string(LENGTH "${bytes}" byte_count)
    if(byte_count EQUAL 0)
        message(FATAL_ERROR "Shader artifact is empty: ${input}")
    endif()
    string(REGEX REPLACE "(..)" "0x\\1," bytes "${bytes}")
    set(${output_variable}
        "alignas(4) const std::uint8_t ${name}[] = {${bytes}};\n"
        PARENT_SCOPE)
endfunction()

prismatix_binary_array("${INPUT_SPIRV}" kCompositorSpirv spirv_array)
prismatix_binary_array("${INPUT_DXIL}" kCompositorDxil dxil_array)
prismatix_binary_array("${INPUT_MSL}" kCompositorMsl msl_array)

file(WRITE "${OUTPUT}"
"#include \"Engine/Graphics/BuiltInShaders.h\"\n"
"#include <SDL3/SDL_gpu.h>\n\n"
"namespace px::graphics {\nnamespace {\n"
"${spirv_array}${dxil_array}${msl_array}"
"}  // namespace\n\n"
"std::optional<ShaderBytecodeView> SelectBuiltInCompositorShader(\n"
"    const std::uint32_t supportedFormats) {\n"
"    if ((supportedFormats & SDL_GPU_SHADERFORMAT_SPIRV) != 0)\n"
"        return ShaderBytecodeView{kCompositorSpirv, sizeof(kCompositorSpirv),\n"
"                                  SDL_GPU_SHADERFORMAT_SPIRV, \"main\"};\n"
"    if ((supportedFormats & SDL_GPU_SHADERFORMAT_DXIL) != 0)\n"
"        return ShaderBytecodeView{kCompositorDxil, sizeof(kCompositorDxil),\n"
"                                  SDL_GPU_SHADERFORMAT_DXIL, \"main\"};\n"
"    if ((supportedFormats & SDL_GPU_SHADERFORMAT_MSL) != 0)\n"
"        return ShaderBytecodeView{kCompositorMsl, sizeof(kCompositorMsl),\n"
"                                  SDL_GPU_SHADERFORMAT_MSL, \"main0\"};\n"
"    return std::nullopt;\n}\n\n}  // namespace px::graphics\n")
