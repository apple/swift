# The swift-frontend tool


# Add additional libraries to which we need to link when the Swift Swift
# parser is built in.
function(add_swift_parser_link_libraries target)
  if(SWIFT_SWIFT_PARSER)
    target_link_libraries(${target}
                          PRIVATE swiftCore)

    get_property(SWIFT_MACRO_PLUGINS GLOBAL PROPERTY SWIFT_MACRO_PLUGINS)
    foreach(macrolib ${SWIFT_MACRO_PLUGINS})
      add_dependencies(${target} ${macrolib})
    endforeach()
  endif()
endfunction()

if(BOOTSTRAPPING_MODE MATCHES "BOOTSTRAPPING.*")

  # Bootstrapping - level 0
  # For more information on how bootstrapping works, see docs/SwiftInTheCompiler.md

  add_swift_host_tool(swift-frontend-bootstrapping0
    driver.cpp
    SWIFT_COMPONENT no_component
    HAS_SWIFT_MODULES
    BOOTSTRAPPING 0
    THINLTO_LD64_ADD_FLTO_CODEGEN_ONLY
  )
  target_link_libraries(swift-frontend-bootstrapping0
                        PRIVATE
                          swiftDriverTool
                          swiftCompilerStub)

  add_swift_parser_link_libraries(swift-frontend-bootstrapping0)

  swift_create_post_build_symlink(swift-frontend-bootstrapping0
    SOURCE "swift-frontend${CMAKE_EXECUTABLE_SUFFIX}"
    DESTINATION "swiftc${CMAKE_EXECUTABLE_SUFFIX}"
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/bootstrapping0/${CMAKE_CFG_INTDIR}/bin")

  # Bootstrapping - level 1

  add_swift_host_tool(swift-frontend-bootstrapping1
    driver.cpp
    SWIFT_COMPONENT no_component
    HAS_SWIFT_MODULES
    BOOTSTRAPPING 1
    THINLTO_LD64_ADD_FLTO_CODEGEN_ONLY
  )
  target_link_libraries(swift-frontend-bootstrapping1
                        PRIVATE
                          swiftDriverTool
                          swiftCompilerModules-bootstrapping1)

  add_swift_parser_link_libraries(swift-frontend-bootstrapping1)

  swift_create_post_build_symlink(swift-frontend-bootstrapping1
    SOURCE "swift-frontend${CMAKE_EXECUTABLE_SUFFIX}"
    DESTINATION "swiftc${CMAKE_EXECUTABLE_SUFFIX}"
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/bootstrapping1/${CMAKE_CFG_INTDIR}/bin")
endif()

add_swift_host_tool(swift-frontend
  driver.cpp
  SWIFT_COMPONENT compiler
  HAS_SWIFT_MODULES
)
target_link_libraries(swift-frontend
                      PUBLIC
                        swiftDriverTool
                        swiftCompilerModules)

add_swift_parser_link_libraries(swift-frontend)

# Create a `swift-driver` executable adjacent to the `swift-frontend` executable
# to ensure that `swiftc` forwards to the standalone driver when invoked.
swift_create_early_driver_copies(swift-frontend)

swift_create_post_build_symlink(swift-frontend
  SOURCE "swift-frontend${CMAKE_EXECUTABLE_SUFFIX}"
  DESTINATION "swift${CMAKE_EXECUTABLE_SUFFIX}"
  WORKING_DIRECTORY "${SWIFT_RUNTIME_OUTPUT_INTDIR}")

swift_create_post_build_symlink(swift-frontend
  SOURCE "swift-frontend${CMAKE_EXECUTABLE_SUFFIX}"
  DESTINATION "swiftc${CMAKE_EXECUTABLE_SUFFIX}"
  WORKING_DIRECTORY "${SWIFT_RUNTIME_OUTPUT_INTDIR}")

swift_create_post_build_symlink(swift-frontend
  SOURCE "swift-frontend${CMAKE_EXECUTABLE_SUFFIX}"
  DESTINATION "sil-opt${CMAKE_EXECUTABLE_SUFFIX}"
  WORKING_DIRECTORY "${SWIFT_RUNTIME_OUTPUT_INTDIR}")

swift_create_post_build_symlink(swift-frontend
  SOURCE "swift-frontend${CMAKE_EXECUTABLE_SUFFIX}"
  DESTINATION "sil-func-extractor${CMAKE_EXECUTABLE_SUFFIX}"
  WORKING_DIRECTORY "${SWIFT_RUNTIME_OUTPUT_INTDIR}")

swift_create_post_build_symlink(swift-frontend
  SOURCE "swift-frontend${CMAKE_EXECUTABLE_SUFFIX}"
  DESTINATION "sil-nm${CMAKE_EXECUTABLE_SUFFIX}"
  WORKING_DIRECTORY "${SWIFT_RUNTIME_OUTPUT_INTDIR}")

swift_create_post_build_symlink(swift-frontend
  SOURCE "swift-frontend${CMAKE_EXECUTABLE_SUFFIX}"
  DESTINATION "sil-llvm-gen${CMAKE_EXECUTABLE_SUFFIX}"
  WORKING_DIRECTORY "${SWIFT_RUNTIME_OUTPUT_INTDIR}")

swift_create_post_build_symlink(swift-frontend
  SOURCE "swift-frontend${CMAKE_EXECUTABLE_SUFFIX}"
  DESTINATION "sil-passpipeline-dumper${CMAKE_EXECUTABLE_SUFFIX}"
  WORKING_DIRECTORY "${SWIFT_RUNTIME_OUTPUT_INTDIR}")

swift_create_post_build_symlink(swift-frontend
  SOURCE "swift-frontend${CMAKE_EXECUTABLE_SUFFIX}"
  DESTINATION "swift-dependency-tool${CMAKE_EXECUTABLE_SUFFIX}"
  WORKING_DIRECTORY "${SWIFT_RUNTIME_OUTPUT_INTDIR}")

swift_create_post_build_symlink(swift-frontend
  SOURCE "swift-frontend${CMAKE_EXECUTABLE_SUFFIX}"
  DESTINATION "swift-llvm-opt${CMAKE_EXECUTABLE_SUFFIX}"
  WORKING_DIRECTORY "${SWIFT_RUNTIME_OUTPUT_INTDIR}")

swift_create_post_build_symlink(swift-frontend
  SOURCE "swift-frontend${CMAKE_EXECUTABLE_SUFFIX}"
  DESTINATION "swift-indent${CMAKE_EXECUTABLE_SUFFIX}"
  WORKING_DIRECTORY "${SWIFT_RUNTIME_OUTPUT_INTDIR}")

swift_create_post_build_symlink(swift-frontend
  SOURCE "swift-frontend${CMAKE_EXECUTABLE_SUFFIX}"
  DESTINATION "swift-symbolgraph-extract${CMAKE_EXECUTABLE_SUFFIX}"
  WORKING_DIRECTORY "${SWIFT_RUNTIME_OUTPUT_INTDIR}")

swift_create_post_build_symlink(swift-frontend
  SOURCE "swift-frontend${CMAKE_EXECUTABLE_SUFFIX}"
  DESTINATION "swift-api-extract${CMAKE_EXECUTABLE_SUFFIX}"
  WORKING_DIRECTORY "${SWIFT_RUNTIME_OUTPUT_INTDIR}")

swift_create_post_build_symlink(swift-frontend
  SOURCE "swift-frontend${CMAKE_EXECUTABLE_SUFFIX}"
  DESTINATION "swift-autolink-extract${CMAKE_EXECUTABLE_SUFFIX}"
  WORKING_DIRECTORY "${SWIFT_RUNTIME_OUTPUT_INTDIR}")

swift_create_post_build_symlink(swift-frontend
  SOURCE "swift-frontend${CMAKE_EXECUTABLE_SUFFIX}"
  DESTINATION "swift-api-digester${CMAKE_EXECUTABLE_SUFFIX}"
  WORKING_DIRECTORY "${SWIFT_RUNTIME_OUTPUT_INTDIR}")

swift_create_post_build_symlink(swift-frontend
  SOURCE "swift-frontend${CMAKE_EXECUTABLE_SUFFIX}"
  DESTINATION "swift-cache-tool${CMAKE_EXECUTABLE_SUFFIX}"
  WORKING_DIRECTORY "${SWIFT_RUNTIME_OUTPUT_INTDIR}")

add_swift_tool_symlink(swift swift-frontend compiler)
add_swift_tool_symlink(swiftc swift-frontend compiler)
add_swift_tool_symlink(swift-symbolgraph-extract swift-frontend compiler)
add_swift_tool_symlink(swift-api-extract swift-frontend compiler)
add_swift_tool_symlink(swift-autolink-extract swift-frontend autolink-driver)
add_swift_tool_symlink(swift-indent swift-frontend editor-integration)
add_swift_tool_symlink(swift-api-digester swift-frontend compiler)
add_swift_tool_symlink(swift-cache-tool swift-frontend compiler)

add_dependencies(compiler swift-frontend)
swift_install_in_component(FILES "${SWIFT_RUNTIME_OUTPUT_INTDIR}/swift${CMAKE_EXECUTABLE_SUFFIX}"
                           DESTINATION "bin"
                           COMPONENT compiler)
swift_install_in_component(FILES "${SWIFT_RUNTIME_OUTPUT_INTDIR}/swiftc${CMAKE_EXECUTABLE_SUFFIX}"
                           DESTINATION "bin"
                           COMPONENT compiler)
swift_install_in_component(FILES "${SWIFT_RUNTIME_OUTPUT_INTDIR}/swift-symbolgraph-extract${CMAKE_EXECUTABLE_SUFFIX}"
                           DESTINATION "bin"
                           COMPONENT compiler)
swift_install_in_component(FILES "${SWIFT_RUNTIME_OUTPUT_INTDIR}/swift-api-extract${CMAKE_EXECUTABLE_SUFFIX}"
                           DESTINATION "bin"
                           COMPONENT compiler)
swift_install_in_component(FILES "${SWIFT_RUNTIME_OUTPUT_INTDIR}/swift-api-digester${CMAKE_EXECUTABLE_SUFFIX}"
                           DESTINATION "bin"
                           COMPONENT compiler)
swift_install_in_component(FILES "${SWIFT_RUNTIME_OUTPUT_INTDIR}/swift-cache-tool${CMAKE_EXECUTABLE_SUFFIX}"
                           DESTINATION "bin"
                           COMPONENT compiler)
add_dependencies(autolink-driver swift-frontend)
swift_install_in_component(FILES "${SWIFT_RUNTIME_OUTPUT_INTDIR}/swift-autolink-extract${CMAKE_EXECUTABLE_SUFFIX}"
                           DESTINATION "bin"
                           COMPONENT autolink-driver)
add_dependencies(editor-integration swift-frontend)
swift_install_in_component(FILES "${SWIFT_RUNTIME_OUTPUT_INTDIR}/swift-indent${CMAKE_EXECUTABLE_SUFFIX}"
                           DESTINATION "bin"
                           COMPONENT editor-integration)

