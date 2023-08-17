set(datafiles
  macos4.json
  ios4.json
  tvos4.json
  watchos4.json
  overlay4.json
  macos42.json
  ios42.json
  tvos42.json
  watchos42.json
  overlay42.json
)
set(SWIFTLIB_DIR
    "${CMAKE_BINARY_DIR}/${CMAKE_CFG_INTDIR}/lib/swift")
set(output_dir "${SWIFTLIB_DIR}/migrator")

add_custom_command(
    OUTPUT "${output_dir}"
    COMMAND ${CMAKE_COMMAND} "-E" "make_directory" "${output_dir}")

set(outputs)

foreach(input ${datafiles})
  set(source "${CMAKE_CURRENT_SOURCE_DIR}/${input}")
  set(dest "${output_dir}/${input}")
  if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(CMAKE_SYMLINK_COMMAND copy)
  else()
    set(CMAKE_SYMLINK_COMMAND create_symlink)
  endif()

  add_custom_command(OUTPUT
                       "${output_dir}/${input}"
                     DEPENDS
                       "${CMAKE_CURRENT_SOURCE_DIR}/${input}"
                     COMMAND
                       "${CMAKE_COMMAND}" "-E" "${CMAKE_SYMLINK_COMMAND}" "${source}" "${dest}")
  list(APPEND outputs "${output_dir}/${input}")
endforeach()
list(APPEND outputs "${output_dir}")

add_custom_target("symlink_migrator_data"
    DEPENDS "${output_dir}" "${outputs}"
    COMMENT "Symlinking migrator data to ${output_dir}")

swift_install_in_component(FILES ${datafiles}
                           DESTINATION "lib/swift/migrator"
                           COMPONENT compiler)

add_swift_host_library(swiftMigrator STATIC
  APIDiffMigratorPass.cpp
  EditorAdapter.cpp
  FixitApplyDiagnosticConsumer.cpp
  Migrator.cpp
  MigrationState.cpp
  OptionalTryMigratorPass.cpp
  RewriteBufferEditsReceiver.cpp)
target_link_libraries(swiftMigrator PRIVATE
  swiftIDE)

add_dependencies(swiftMigrator
  "symlink_migrator_data")

set_swift_llvm_is_available(swiftMigrator)
