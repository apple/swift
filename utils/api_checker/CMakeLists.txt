set(framework "FrameworkABIBaseline")
swift_install_in_component(PROGRAMS "swift-api-checker.py"
                           DESTINATION "bin"
                           COMPONENT toolchain-tools)
swift_install_in_component(DIRECTORY "sdk-module-lists"
                           DESTINATION "bin"
                           COMPONENT toolchain-tools)
swift_install_in_component(DIRECTORY "${framework}"
                           DESTINATION "lib/swift"
                           COMPONENT toolchain-tools)

# Add symlink of FrameworkABIBaseline to the build dir. This ensures we can
# find the baseline data from the same relative path as if we are running the
# checker from the toolchain.
set(SWIFTLIB_DIR "${CMAKE_BINARY_DIR}/${CMAKE_CFG_INTDIR}/lib/swift")
set(dest "${SWIFTLIB_DIR}/${framework}")
set(source "${CMAKE_CURRENT_SOURCE_DIR}/${framework}")

if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(CMAKE_SYMLINK_COMMAND copy)
else()
    set(CMAKE_SYMLINK_COMMAND create_symlink)
endif()
add_custom_command(OUTPUT "${dest}"
                   DEPENDS "${source}"
                   COMMAND "${CMAKE_COMMAND}" "-E" "${CMAKE_SYMLINK_COMMAND}" "${source}" "${dest}"
                   COMMENT "Symlinking ABI checker baseline data to ${dest}")
add_custom_target("symlink_abi_checker_data" ALL
                  DEPENDS "${dest}"
                  COMMENT "Symlinking ABI checker baseline data to ${dest}")
if(TARGET swift-frontend)
  add_dependencies(swift-frontend symlink_abi_checker_data)
endif()
