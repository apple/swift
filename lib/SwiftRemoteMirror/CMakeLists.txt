add_swift_host_library(swiftRemoteMirror STATIC
  ${SWIFT_SOURCE_DIR}/stdlib/public/SwiftRemoteMirror/SwiftRemoteMirror.cpp)
target_link_libraries(swiftRemoteMirror PRIVATE
  swiftDemangling)

if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
  target_compile_definitions(swiftRemoteMirror PRIVATE _LIB)
endif()
