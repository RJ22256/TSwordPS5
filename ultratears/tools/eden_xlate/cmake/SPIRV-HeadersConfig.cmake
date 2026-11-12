# The SPIR-V headers MSYS2 CLANG64 installs (include/spirv) without a CMake package: what sirit's
# find_package(SPIRV-Headers) needs.
if(NOT TARGET SPIRV-Headers::SPIRV-Headers)
  add_library(SPIRV-Headers::SPIRV-Headers INTERFACE IMPORTED)
  set_target_properties(SPIRV-Headers::SPIRV-Headers PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "C:/msys64/clang64/include")
endif()
set(SPIRV-Headers_FOUND TRUE)
