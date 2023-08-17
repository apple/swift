add_sourcekit_executable(sourcekitd-repl
  sourcekitd-repl.cpp
  LLVM_LINK_COMPONENTS coverage lto
)
if(SWIFT_SOURCEKIT_USE_INPROC_LIBRARY)
  target_link_libraries(sourcekitd-repl PRIVATE sourcekitdInProc)
else()
  target_link_libraries(sourcekitd-repl PRIVATE sourcekitd)
endif()
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  target_link_libraries(sourcekitd-repl PRIVATE
    dispatch
    BlocksRuntime)
endif()
target_link_libraries(sourcekitd-repl PRIVATE
  LibEdit::LibEdit)

if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  set_target_properties(sourcekitd-repl PROPERTIES
    INSTALL_RPATH "@executable_path/../lib")
  target_link_options(sourcekitd-repl PRIVATE
    "LINKER:-exported_symbol,_main")
endif()
if(SWIFT_ANALYZE_CODE_COVERAGE)
  set_property(TARGET sourcekitd-repl APPEND_STRING PROPERTY
    LINK_FLAGS " -fprofile-instr-generate -fcoverage-mapping")
endif()

add_dependencies(tools sourcekitd-repl)
swift_install_in_component(TARGETS sourcekitd-repl
  RUNTIME DESTINATION bin COMPONENT tools)
