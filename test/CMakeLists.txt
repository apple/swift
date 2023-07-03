list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/modules")

include(SwiftTestUtils)

function(swift_configure_lit_site_cfg source_path destination_path installed_name)
  if (CMAKE_CFG_INTDIR STREQUAL ".")
    set(SWIFT_BUILD_MODE ".")
  else ()
    set(SWIFT_BUILD_MODE "%(build_mode)s")
  endif ()

  string(REPLACE ${CMAKE_CFG_INTDIR} ${SWIFT_BUILD_MODE} LLVM_TOOLS_DIR ${LLVM_TOOLS_BINARY_DIR})
  string(REPLACE ${CMAKE_CFG_INTDIR} ${SWIFT_BUILD_MODE} LLVM_LIBS_DIR  ${LLVM_LIBRARY_DIR})

  if (XCODE)
    string(REPLACE ${CMAKE_CFG_INTDIR} Debug LIT_SWIFTLIB_DIR ${SWIFTLIB_DIR})
  else ()
    set(LIT_SWIFTLIB_DIR ${SWIFTLIB_DIR})
  endif ()

  configure_file("${source_path}" "${destination_path}" @ONLY)

  if(NOT "${installed_name}" STREQUAL "")
    swift_install_in_component(FILES "${destination_path}"
                               RENAME "${installed_name}"
                               DESTINATION "share/swift/testsuite"
                               COMPONENT testsuite-tools)
  endif()
endfunction()

function(normalize_boolean_spelling var_name)
  if(${var_name})
    set("${var_name}" TRUE PARENT_SCOPE)
  else()
    set("${var_name}" FALSE PARENT_SCOPE)
  endif()
endfunction()

function(get_test_dependencies SDK result_var_name)
  set(deps)

  if(SWIFT_BUILD_STDLIB)
    list(APPEND deps SwiftUnitTests)
  endif()

  if(SWIFT_BUILD_SDK_OVERLAY OR SWIFT_BUILD_TEST_SUPPORT_MODULES)
    list(APPEND deps sdk-overlay)
  endif()

  set(deps_binaries)

  if (SWIFT_INCLUDE_TOOLS)
    list(APPEND deps_binaries
      lldb-moduleimport-test
      swift-frontend
      swift-demangle
      swift-demangle-yamldump
      swift-ide-test
      swift-refactor
      swift-reflection-dump
      swift-remoteast-test)

    if(SWIFT_BUILD_SOURCEKIT)
      list(APPEND deps_binaries sourcekitd-test complete-test)
    endif()
  endif()

  if(NOT SWIFT_BUILT_STANDALONE)
    list(APPEND deps_binaries
      arcmt-test
      c-arcmt-test
      c-index-test
      CASPluginTest
      clang
      clang-cas-test
      count
      dsymutil
      FileCheck
      llc
      llvm-ar
      llvm-as
      llvm-bcanalyzer
      llvm-cas
      llvm-cov
      llvm-dis
      llvm-dwarfdump
      llvm-link
      llvm-nm
      llvm-objdump
      llvm-profdata
      llvm-readelf
      llvm-readobj
      llvm-size
      llvm-strings
      not
      split-file)
  endif()

  if(("${SDK}" STREQUAL "IOS") OR
     ("${SDK}" STREQUAL "TVOS") OR
     ("${SDK}" STREQUAL "WATCHOS") OR
     ("${SDK}" STREQUAL "OSX") OR
     ("${SDK}" STREQUAL "IOS_SIMULATOR") OR
     ("${SDK}" STREQUAL "TVOS_SIMULATOR") OR
     ("${SDK}" STREQUAL "WATCHOS_SIMULATOR") OR
     ("${SDK}" STREQUAL "FREESTANDING") OR
     ("${SDK}" STREQUAL "LINUX") OR
     ("${SDK}" STREQUAL "CYGWIN") OR
     ("${SDK}" STREQUAL "FREEBSD") OR
     ("${SDK}" STREQUAL "OPENBSD") OR
     ("${SDK}" STREQUAL "ANDROID") OR
     ("${SDK}" STREQUAL "WINDOWS") OR
     ("${SDK}" STREQUAL "HAIKU"))
    # No extra dependencies.
  else()
    message(FATAL_ERROR "Unknown SDK: ${SDK}")
  endif()

  # Just use target names for dependency generation. This works for both Xcode
  # and non-Xcode build systems. In the case of Xcode, its build paths have a
  # configuration variable in them, so CMake can't match them at compile time.
  list(APPEND deps ${deps_binaries})

  set("${result_var_name}" "${deps}" PARENT_SCOPE)
endfunction()

set(LIT "${LLVM_MAIN_SRC_DIR}/utils/lit/lit.py")

set(SWIFT_LIT_ARGS "" CACHE STRING "Arguments to pass to lit")

set(SWIFT_LIT_ENVIRONMENT "" CACHE STRING "Environment to use for lit invocations")

if(NOT SWIFT_INCLUDE_TOOLS)
  if(SWIFT_RUN_TESTS_WITH_HOST_COMPILER)
    precondition(CMAKE_Swift_COMPILER MESSAGE "Can only run tests if a Swift compiler is specified")
    get_filename_component(SWIFT_COMPILER_DIR "${CMAKE_Swift_COMPILER}" DIRECTORY)
    precondition(SWIFT_COMPILER_DIR)
    # We assume that we are building against a toolchain where all tools are
    # next to swiftc.
    list(APPEND SWIFT_LIT_ARGS
      "--path=${SWIFT_COMPILER_DIR}")
  else()
    list(APPEND SWIFT_LIT_ARGS
      "--path=${SWIFT_NATIVE_LLVM_TOOLS_PATH}"
      "--param" "swift_native_llvm_tools_path=${SWIFT_NATIVE_LLVM_TOOLS_PATH}"
      "--path=${SWIFT_NATIVE_CLANG_TOOLS_PATH}"
      "--param" "swift_native_clang_tools_path=${SWIFT_NATIVE_CLANG_TOOLS_PATH}"
      "--path=${SWIFT_NATIVE_SWIFT_TOOLS_PATH}"
      "--param" "swift_native_swift_tools_path=${SWIFT_NATIVE_SWIFT_TOOLS_PATH}"
      )
  endif()
  if(SWIFT_BUILD_STDLIB)
    list(APPEND SWIFT_LIT_ARGS
         "--param" "test_resource_dir=${SWIFTLIB_DIR}")
  endif()
endif()

option(SWIFT_TEST_USE_LEAKS "Run Swift stdlib tests under leaks" FALSE)
if (SWIFT_TEST_USE_LEAKS)
  list(APPEND SWIFT_LIT_ARGS "--param" "leaks-all")
endif()

if (SWIFT_ENABLE_ARRAY_COW_CHECKS)
  list(APPEND SWIFT_LIT_ARGS
       "--param" "array_cow_checks")
endif()

if(NOT CMAKE_CFG_INTDIR STREQUAL ".")
  list(APPEND SWIFT_LIT_ARGS
       "--param" "build_mode=${CMAKE_CFG_INTDIR}")
endif()

if (LLVM_USE_SANITIZER STREQUAL "Address")
  set(SWIFT_ASAN_BUILD TRUE)
endif()

# Normalize spelling of boolean values.
normalize_boolean_spelling(LLVM_ENABLE_ASSERTIONS)
normalize_boolean_spelling(SWIFT_STDLIB_ASSERTIONS)
normalize_boolean_spelling(SWIFT_AST_VERIFIER)
normalize_boolean_spelling(SWIFT_ASAN_BUILD)
normalize_boolean_spelling(SWIFT_ENABLE_SOURCEKIT_TESTS)
normalize_boolean_spelling(SWIFT_ENABLE_EXPERIMENTAL_DIFFERENTIABLE_PROGRAMMING)
normalize_boolean_spelling(SWIFT_ENABLE_EXPERIMENTAL_CONCURRENCY)
normalize_boolean_spelling(SWIFT_BACK_DEPLOY_CONCURRENCY)
normalize_boolean_spelling(SWIFT_ENABLE_EXPERIMENTAL_DISTRIBUTED)
normalize_boolean_spelling(SWIFT_ENABLE_EXPERIMENTAL_STRING_PROCESSING)
normalize_boolean_spelling(SWIFT_ENABLE_EXPERIMENTAL_OBSERVATION)
normalize_boolean_spelling(SWIFT_ENABLE_MACCATALYST)
normalize_boolean_spelling(SWIFT_RUN_TESTS_WITH_HOST_COMPILER)
normalize_boolean_spelling(SWIFT_RUNTIME_ENABLE_LEAK_CHECKER)
normalize_boolean_spelling(SWIFT_OPTIMIZED)
normalize_boolean_spelling(SWIFT_STDLIB_SINGLE_THREADED_CONCURRENCY)
normalize_boolean_spelling(SWIFT_ENABLE_REFLECTION)
normalize_boolean_spelling(SWIFT_ENABLE_RUNTIME_FUNCTION_COUNTERS)
normalize_boolean_spelling(SWIFT_STDLIB_ENABLE_DEBUG_PRECONDITIONS_IN_RELEASE)
normalize_boolean_spelling(SWIFT_HAVE_LIBXML2)
normalize_boolean_spelling(SWIFT_INCLUDE_TOOLS)
normalize_boolean_spelling(SWIFT_STDLIB_STATIC_PRINT)
normalize_boolean_spelling(SWIFT_STDLIB_ENABLE_UNICODE_DATA)
normalize_boolean_spelling(SWIFT_ENABLE_DISPATCH)
normalize_boolean_spelling(SWIFT_STDLIB_ENABLE_OBJC_INTEROP)
normalize_boolean_spelling(SWIFT_ENABLE_BACKTRACING)
is_build_type_optimized("${SWIFT_STDLIB_BUILD_TYPE}" SWIFT_OPTIMIZED)

# Get 'SWIFT_HOST_TRIPLE' and 'SWIFT_HOST_SDKROOT' for lit.site.cfg.in
if(SWIFT_HOST_VARIANT_SDK IN_LIST SWIFT_DARWIN_PLATFORMS)
  set(DEPLOYMENT_VERSION "${SWIFT_SDK_${SWIFT_HOST_VARIANT_SDK}_DEPLOYMENT_VERSION}")
endif()
if(SWIFT_HOST_VARIANT_SDK STREQUAL ANDROID)
  set(DEPLOYMENT_VERSION ${SWIFT_ANDROID_API_LEVEL})
endif()
get_target_triple(SWIFT_HOST_TRIPLE SWIFT_HOST_TRIPLE_VARIANT "${SWIFT_HOST_VARIANT_SDK}" "${SWIFT_HOST_VARIANT_ARCH}"
  MACCATALYST_BUILD_FLAVOR ""
  DEPLOYMENT_VERSION "${DEPLOYMENT_VERSION}")
set(SWIFT_HOST_SDKROOT "${SWIFT_SDK_${SWIFT_HOST_VARIANT_SDK}_ARCH_${SWIFT_HOST_VARIANT_ARCH}_PATH}")

set(profdata_merge_worker
    "${CMAKE_CURRENT_SOURCE_DIR}/../utils/profdata_merge/main.py")

set(TEST_MODES
    optimize_none optimize optimize_unchecked optimize_size
    optimize_none_with_implicit_dynamic
    optimize_with_implicit_dynamic
    only_executable only_non_executable
    with_cxx_interop
)
set(TEST_SUBSETS
    primary
    validation
    all
    only_validation
    only_long
    only_stress
    only_early_swiftdriver
)

if(NOT "${COVERAGE_DB}" STREQUAL "")
  add_custom_target("touch-covering-tests"
      COMMAND "${SWIFT_SOURCE_DIR}/utils/coverage/coverage-touch-tests" "--swift-dir" "${SWIFT_SOURCE_DIR}" "--coverage-db" "${COVERAGE_DB}"
      COMMENT "Touching covering tests")
endif()

foreach(SDK ${SWIFT_SDKS})
  foreach(ARCH ${SWIFT_SDK_${SDK}_ARCHITECTURES})
    # macCatalyst needs to run two sets of tests: one with the normal macosx target triple
    # and one with the macCatalyst ios-macabi triple.  The build_flavors list will
    # have only the "default" flavor for all SDKs and architectures except
    # OSX when macCatalyst support is enabled.
    get_swift_test_build_flavors(build_flavors "${SDK}")

    foreach(BUILD_FLAVOR ${build_flavors})
      # Configure variables for this subdirectory.
      set(VARIANT_SDK "${SWIFT_SDK_${SDK}_ARCH_${ARCH}_PATH}")
      get_swift_test_variant_suffix(VARIANT_SUFFIX "${SDK}" "${ARCH}" "${BUILD_FLAVOR}")
      get_swift_test_variant_suffix(DEFAULT_OSX_VARIANT_SUFFIX "${SDK}" "${ARCH}" "default")
      get_swift_test_versioned_target_triple(VARIANT_TRIPLE "${SDK}" "${ARCH}" "${SWIFT_SDK_${SDK}_DEPLOYMENT_VERSION}" "${BUILD_FLAVOR}")

      # A directory where to put the xUnit-style XML test results.
      set(SWIFT_TEST_RESULTS_DIR
          "${CMAKE_BINARY_DIR}/${CMAKE_CFG_INTDIR}/swift-test-results/${VARIANT_TRIPLE}")

      set(command_clean_test_results_dir
          COMMAND "${CMAKE_COMMAND}" -E remove_directory "${SWIFT_TEST_RESULTS_DIR}"
          COMMAND "${CMAKE_COMMAND}" -E make_directory "${SWIFT_TEST_RESULTS_DIR}")

      set(test_bin_dir "${CMAKE_CURRENT_BINARY_DIR}${VARIANT_SUFFIX}")
      set(validation_test_bin_dir
          "${CMAKE_CURRENT_BINARY_DIR}/../validation-test${VARIANT_SUFFIX}")

      if(LLVM_ENABLE_LIBXML2)
        set(SWIFT_HAVE_LIBXML2 TRUE)
      else()
        set(SWIFT_HAVE_LIBXML2 FALSE)
      endif()

      swift_configure_lit_site_cfg(
          "${CMAKE_CURRENT_SOURCE_DIR}/lit.site.cfg.in"
          "${test_bin_dir}/lit.site.cfg"
          "test${VARIANT_SUFFIX}.lit.site.cfg")

      swift_configure_lit_site_cfg(
          "${CMAKE_CURRENT_SOURCE_DIR}/Unit/lit.site.cfg.in"
          "${test_bin_dir}/Unit/lit.site.cfg"
          "")

      swift_configure_lit_site_cfg(
          "${CMAKE_CURRENT_SOURCE_DIR}/../validation-test/lit.site.cfg.in"
          "${validation_test_bin_dir}/lit.site.cfg"
          "validation-test${VARIANT_SUFFIX}.lit.site.cfg")

      set(test_dependencies)
      get_test_dependencies("${SDK}" test_dependencies)

      # Keep in sync with stdlib/tools/CMakeLists.txt: swift-reflection-test is
      # only used when testing dynamic stdlib.
      if(SWIFT_BUILD_DYNAMIC_STDLIB AND SWIFT_INCLUDE_TESTS)
        list(APPEND test_dependencies BlocksRuntimeStub${VARIANT_SUFFIX})

        list(APPEND test_dependencies
            "swift-test-stdlib-${SWIFT_SDK_${SDK}_LIB_SUBDIR}")

        if(BUILD_FLAVOR STREQUAL "ios-like")
          # When testing the iOS-like build flavor, use the normal macOS
          # swift-reflection-test-tool. That tool runs out of process so it
          # doesn't need to be build for macCatalyst.
          list(APPEND test_dependencies
              "swift-reflection-test${DEFAULT_OSX_VARIANT_SUFFIX}")
        else()
          list(APPEND test_dependencies
              "swift-reflection-test${VARIANT_SUFFIX}")
        endif()
      endif()

      if(NOT "${COVERAGE_DB}" STREQUAL "")
        list(APPEND test_dependencies "touch-covering-tests")
      endif()

      list(APPEND test_dependencies
        "swiftStdlibCollectionUnittest-${SWIFT_SDK_${SDK}_LIB_SUBDIR}"
        "swiftStdlibUnicodeUnittest-${SWIFT_SDK_${SDK}_LIB_SUBDIR}")

      set(validation_test_dependencies)

      set(command_upload_stdlib)
      set(command_upload_swift_reflection_test)
      if("${SDK}" STREQUAL "IOS" OR "${SDK}" STREQUAL "TVOS" OR "${SDK}" STREQUAL "WATCHOS")
        # These are supported testing SDKs, but their implementation of
        # `command_upload_stdlib` is hidden.
      elseif("${SDK}" STREQUAL "ANDROID" AND NOT "${SWIFT_HOST_VARIANT}" STREQUAL "android")
        # This adb setup is only needed when cross-compiling for Android, so the
        # second check above makes sure we don't bother when the host is Android.
        if("${SWIFT_ANDROID_DEPLOY_DEVICE_PATH}" STREQUAL "")
          message(FATAL_ERROR
              "When running Android host tests, you must specify the directory on the device "
              "to which Swift build products will be deployed.")
        endif()

        # Warning: This step will fail if you do not have an Android device
        #          connected via USB. See docs/Android.md for details on
        #          how to run the test suite for Android.
        set(command_upload_stdlib
            COMMAND
                # Reboot the device and remove everything in its tmp
                # directory. Build products and test executables are pushed
                # to that directory when running the test suite.
                $<TARGET_FILE:Python3::Interpreter> "${SWIFT_SOURCE_DIR}/utils/android/adb_clean.py"
            COMMAND
                $<TARGET_FILE:Python3::Interpreter> "${SWIFT_SOURCE_DIR}/utils/android/adb_push_built_products.py"
                --ndk "${SWIFT_ANDROID_NDK_PATH}"
                --destination "${SWIFT_ANDROID_DEPLOY_DEVICE_PATH}"
                --destination-arch "${ARCH}"
                # Build products like libswiftCore.so.
                "${SWIFTLIB_DIR}/android")
      endif()
      add_custom_target("upload-stdlib${VARIANT_SUFFIX}"
          ${command_upload_stdlib}
          ${command_upload_swift_reflection_test}
          COMMENT "Uploading stdlib")

      foreach(test_mode ${TEST_MODES})
        set(LIT_ARGS "${SWIFT_LIT_ARGS} ${LLVM_LIT_ARGS}")
        separate_arguments(LIT_ARGS)

        if(NOT SWIFT_BUILD_STDLIB AND NOT SWIFT_PATH_TO_EXTERNAL_STDLIB_BUILD)
          list(APPEND LIT_ARGS
              "--param" "test_sdk_overlay_dir=${SWIFTLIB_DIR}/${SWIFT_SDK_${SDK}_LIB_SUBDIR}")
        endif()

        execute_process(COMMAND
            $<TARGET_FILE:Python3::Interpreter> "-c" "import psutil"
            RESULT_VARIABLE python_psutil_status
            TIMEOUT 1 # second
            ERROR_QUIET)
        if(NOT python_psutil_status)
          list(APPEND LIT_ARGS "--timeout=3000") # 50 minutes
        endif()

        list(APPEND LIT_ARGS "--xunit-xml-output=${SWIFT_TEST_RESULTS_DIR}/lit-tests.xml")

        if(SWIFT_ENABLE_EXPERIMENTAL_DIFFERENTIABLE_PROGRAMMING)
          list(APPEND LIT_ARGS "--param" "differentiable_programming")
        endif()

        if(SWIFT_ENABLE_EXPERIMENTAL_CONCURRENCY)
          list(APPEND LIT_ARGS "--param" "concurrency")
        endif()

        if(SWIFT_BACK_DEPLOY_CONCURRENCY)
          list(APPEND LIT_ARGS "--param" "back_deploy_concurrency")
        endif()

        if(SWIFT_ENABLE_EXPERIMENTAL_DISTRIBUTED)
          list(APPEND LIT_ARGS "--param" "distributed")
        endif()

        if(SWIFT_ENABLE_EXPERIMENTAL_STRING_PROCESSING)
          list(APPEND LIT_ARGS "--param" "string_processing")
        endif()

        if(SWIFT_ENABLE_BACKTRACING)
          list(APPEND LIT_ARGS "--param" "backtracing")
        endif()

        if(SWIFT_ENABLE_EXPERIMENTAL_OBSERVATION)
          list(APPEND LIT_ARGS "--param" "observation")
        endif()

        list(APPEND LIT_ARGS "--param" "threading=${SWIFT_SDK_${SDK}_THREADING_PACKAGE}")

        # Enable on-crash backtracing if supported
        if(("${SDK}" STREQUAL "OSX" OR "${SDK}" STREQUAL "LINUX")
            AND NOT SWIFT_ASAN_BUILD)
          list(APPEND LIT_ARGS "--param" "backtrace_on_crash")
        endif()

        foreach(test_subset ${TEST_SUBSETS})
          set(directories)
          set(dependencies ${test_dependencies})

          if((test_subset STREQUAL "primary") OR
             (test_subset STREQUAL "validation") OR
             (test_subset STREQUAL "only_long") OR
             (test_subset STREQUAL "only_stress") OR
             (test_subset STREQUAL "only_early_swiftdriver") OR
             (test_subset STREQUAL "all"))
            list(APPEND directories "${test_bin_dir}")
          endif()
          if((test_subset STREQUAL "validation") OR
             (test_subset STREQUAL "only_validation") OR
             (test_subset STREQUAL "only_long") OR
             (test_subset STREQUAL "only_stress") OR
             (test_subset STREQUAL "all"))
            list(APPEND directories "${validation_test_bin_dir}")
            list(APPEND dependencies ${validation_test_dependencies})
          endif()

          if("${SWIFT_SDK_${SDK}_OBJECT_FORMAT}" STREQUAL "ELF")
            list(APPEND dependencies swiftImageRegistration${VARIANT_SUFFIX})
          endif()

          set(test_subset_target_suffix "-${test_subset}")
          if(test_subset STREQUAL "primary")
            set(test_subset_target_suffix "")
          endif()

          set(test_mode_target_suffix "")
          if(NOT test_mode STREQUAL "optimize_none")
            set(test_mode_target_suffix "-${test_mode}")
          endif()

          set(maybe_command_upload_stdlib)
          if(NOT test_mode STREQUAL "only_non_executable")
            set(maybe_command_upload_stdlib ${command_upload_stdlib})
          endif()

          set(test_target_name
              "check-swift${test_subset_target_suffix}${test_mode_target_suffix}${VARIANT_SUFFIX}")
          add_custom_target("${test_target_name}"
              ${maybe_command_upload_stdlib}
              ${command_upload_swift_reflection_test}
              ${command_clean_test_results_dir}
              COMMAND
                ${CMAKE_COMMAND} -E env ${SWIFT_LIT_ENVIRONMENT}
                $<TARGET_FILE:Python3::Interpreter> "${LIT}"
                ${LIT_ARGS}
                "--param" "swift_test_subset=${test_subset}"
                "--param" "swift_test_mode=${test_mode}"
                ${directories}
              DEPENDS ${dependencies}
              COMMENT "Running ${test_subset} Swift tests for ${VARIANT_TRIPLE}"
              USES_TERMINAL)

          set(test_dependencies_target_name
              "swift${test_subset_target_suffix}${test_mode_target_suffix}${VARIANT_SUFFIX}-test-depends")
          add_custom_target("${test_dependencies_target_name}"
              DEPENDS ${dependencies})

          add_custom_target("${test_target_name}-custom"
              ${command_upload_stdlib}
              ${command_upload_swift_reflection_test}
              ${command_clean_test_results_dir}
              COMMAND
                ${CMAKE_COMMAND} -E env ${SWIFT_LIT_ENVIRONMENT}
                $<TARGET_FILE:Python3::Interpreter> "${LIT}"
                ${LIT_ARGS}
                "--param" "swift_test_subset=${test_subset}"
                "--param" "swift_test_mode=${test_mode}"
                ${SWIFT_LIT_TEST_PATHS}
              DEPENDS ${dependencies}
              COMMENT "Running ${test_subset} Swift tests for ${VARIANT_TRIPLE} from custom test locations"
              USES_TERMINAL)
          set_property(TARGET
              "${test_target_name}"
              "${test_target_name}-custom"
              "${test_dependencies_target_name}"
              PROPERTY FOLDER "Tests/check-swift")
        endforeach()
      endforeach()
    endforeach()
  endforeach()
endforeach()

# Add shortcuts for the default variant.
foreach(test_mode ${TEST_MODES})
  foreach(test_subset ${TEST_SUBSETS})
    set(test_mode_target_suffix "")
    if(NOT test_mode STREQUAL "optimize_none")
      set(test_mode_target_suffix "-${test_mode}")
    endif()
    set(test_subset_target_suffix "-${test_subset}")
    if(test_subset STREQUAL "primary")
      set(test_subset_target_suffix "")
    endif()

    set(test_target_name
        "check-swift${test_subset_target_suffix}${test_mode_target_suffix}")
    add_custom_target("${test_target_name}"
        DEPENDS "${test_target_name}${SWIFT_PRIMARY_VARIANT_SUFFIX}")
    set_property(TARGET "${test_target_name}"
        PROPERTY FOLDER "Tests/check-swift")

    set(test_depends_target_name
        "swift${test_subset_target_suffix}${test_mode_target_suffix}-test-depends")
    add_custom_target("${test_depends_target_name}"
        DEPENDS "swift${test_subset_target_suffix}${test_mode_target_suffix}${SWIFT_PRIMARY_VARIANT_SUFFIX}-test-depends")
    set_property(TARGET "${test_depends_target_name}"
        PROPERTY FOLDER "Tests/check-swift")
  endforeach()
endforeach()
