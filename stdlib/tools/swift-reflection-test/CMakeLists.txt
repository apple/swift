add_swift_target_executable(swift-reflection-test BUILD_WITH_STDLIB
  NOSWIFTRT
  swift-reflection-test.c
  overrides.c
  LINK_LIBRARIES
    swiftRemoteMirror
    swiftRemoteInspection

  TARGET_SDKS NOT LINUX_STATIC)

# NOTE(compnerd) since _WINDLL has no impact on non-Windows targets,
# we just spam it on all the targets for simplicity due to the build
# structure of swift.  This will make the global variable imported,
# which we need.
set_source_files_properties(swift-reflection-test.c
                            PROPERTIES
                              COMPILE_FLAGS -D_WINDLL)
