set(LLVM_TARGET_DEFINITIONS Options.td)
swift_tablegen(Options.inc -gen-opt-parser-defs)
swift_add_public_tablegen_target(sourcekitdTestOptionsTableGen)

add_sourcekit_executable(sourcekitd-test
  sourcekitd-test.cpp
  TestOptions.cpp
  LLVM_LINK_COMPONENTS option coverage lto
)
target_link_libraries(sourcekitd-test PRIVATE
  SourceKitSupport
  clangRewrite
  clangLex
  clangBasic)
if(SWIFT_SOURCEKIT_USE_INPROC_LIBRARY)
  target_link_libraries(sourcekitd-test PRIVATE sourcekitdInProc)
else()
  target_link_libraries(sourcekitd-test PRIVATE sourcekitd)
endif()
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  target_link_libraries(sourcekitd-test PRIVATE
    dispatch
    BlocksRuntime)
endif()

add_dependencies(sourcekitd-test sourcekitdTestOptionsTableGen)

if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  set_target_properties(sourcekitd-test PROPERTIES
    INSTALL_RPATH "@executable_path/../lib")
  target_link_options(sourcekitd-test PRIVATE
    "LINKER:-exported_symbol,_main")
endif()
if(SWIFT_ANALYZE_CODE_COVERAGE)
  set_property(TARGET sourcekitd-test APPEND_STRING PROPERTY
    LINK_FLAGS " -fprofile-instr-generate -fcoverage-mapping")
endif()

add_dependencies(tools sourcekitd-test)
swift_install_in_component(TARGETS sourcekitd-test
  RUNTIME DESTINATION bin COMPONENT tools)
