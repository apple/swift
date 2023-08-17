# We can't currently build the runtime tests under ASAN
# The problem is that we want to use the just\build compiler (just like the
# runtime) but ASAN will complain if we link against libgtest and LLVMSupport
# libraries because they were compiled with the host compiler.
if(("${SWIFT_HOST_VARIANT_SDK}" STREQUAL "${SWIFT_PRIMARY_VARIANT_SDK}") AND
  ("${SWIFT_HOST_VARIANT_ARCH}" STREQUAL "${SWIFT_PRIMARY_VARIANT_ARCH}") AND
  (NOT (LLVM_USE_SANITIZER STREQUAL "Address")))

  if("${CMAKE_C_COMPILER_ID}" MATCHES "Clang")
    if(NOT SWIFT_BUILD_RUNTIME_WITH_HOST_COMPILER)
      set(CMAKE_CXX_COMPILER "${SWIFT_NATIVE_CLANG_TOOLS_PATH}/clang++")
      set(CMAKE_C_COMPILER "${SWIFT_NATIVE_CLANG_TOOLS_PATH}/clang")
    endif()
  elseif(SWIFT_BUILD_RUNTIME_WITH_HOST_COMPILER)
    message(FATAL_ERROR "Building the swift runtime is not supported with ${CMAKE_C_COMPILER_ID}. Use the just-built clang instead.")
  else()
    message(WARNING "Building the swift runtime using the host compiler, and not the just-built clang.")

    # If we use Clang-cl or MSVC, CMake provides default compiler and linker flags that are incompatible
    # with the frontend of Clang or Clang++.
    if(SWIFT_COMPILER_IS_MSVC_LIKE)
      set(CMAKE_CXX_COMPILER "${SWIFT_NATIVE_CLANG_TOOLS_PATH}/clang-cl")
      set(CMAKE_C_COMPILER "${SWIFT_NATIVE_CLANG_TOOLS_PATH}/clang-cl")
    else()
      set(CMAKE_CXX_COMPILER "${SWIFT_NATIVE_CLANG_TOOLS_PATH}/clang++")
      set(CMAKE_C_COMPILER "${SWIFT_NATIVE_CLANG_TOOLS_PATH}/clang")
    endif()

    if(CMAKE_C_COMPILER_LAUNCHER MATCHES ".*distcc")
      set(CMAKE_C_COMPILER_LAUNCHER "")
    endif()
    if(CMAKE_CXX_COMPILER_LAUNCHER MATCHES ".*distcc")
      set(CMAKE_CXX_COMPILER_LAUNCHER "")
    endif()
  endif()

  add_subdirectory(LongTests)

  set(PLATFORM_SOURCES)
  set(PLATFORM_TARGET_LINK_LIBRARIES)
  if(SWIFT_HOST_VARIANT MATCHES "${SWIFT_DARWIN_VARIANTS}")
    find_library(FOUNDATION_LIBRARY Foundation)
    list(APPEND PLATFORM_SOURCES
      weak.mm
      Refcounting.mm
      )
    # We need to link swiftCore on Darwin because the runtime still relies on
    # some stdlib hooks to implement SwiftObject.
    list(APPEND PLATFORM_TARGET_LINK_LIBRARIES
      ${FOUNDATION_LIBRARY}
      swift_Concurrency${SWIFT_PRIMARY_VARIANT_SUFFIX}
      swiftStdlibUnittest${SWIFT_PRIMARY_VARIANT_SUFFIX}
      )
  elseif(SWIFT_HOST_VARIANT STREQUAL "Linux")
    if(SWIFT_HOST_VARIANT_ARCH MATCHES "armv6|armv7|i686")
      list(APPEND PLATFORM_TARGET_LINK_LIBRARIES
        "atomic")
    endif()
  elseif(SWIFT_HOST_VARIANT STREQUAL "freebsd")
    find_library(EXECINFO_LIBRARY execinfo)
    list(APPEND PLATFORM_TARGET_LINK_LIBRARIES
      ${EXECINFO_LIBRARY}
      )
  elseif(SWIFT_HOST_VARIANT STREQUAL "windows")
    list(APPEND PLATFORM_TARGET_LINK_LIBRARIES DbgHelp;Synchronization)
  endif()

  if(SWIFT_ENABLE_EXPERIMENTAL_CONCURRENCY)
    list(APPEND PLATFORM_SOURCES
      Actor.cpp
      TaskStatus.cpp
      )
    list(APPEND PLATFORM_TARGET_LINK_LIBRARIES
      swift_Concurrency${SWIFT_PRIMARY_VARIANT_SUFFIX}
      )

    if(NOT "${SWIFT_PRIMARY_VARIANT_SDK}" IN_LIST SWIFT_DARWIN_PLATFORMS)
      list(APPEND PLATFORM_TARGET_LINK_LIBRARIES
        dispatch${SWIFT_PRIMARY_VARIANT_SUFFIX}
        BlocksRuntime${SWIFT_PRIMARY_VARIANT_SUFFIX}
        )
    endif()
  endif()

  if(SWIFT_ENABLE_EXPERIMENTAL_DISTRIBUTED)
#    list(APPEND PLATFORM_SOURCES
#      DistributedActor.cpp
#      )
    list(APPEND PLATFORM_TARGET_LINK_LIBRARIES
      swiftDistributed${SWIFT_PRIMARY_VARIANT_SUFFIX}
      )
  endif()

  # Don't complain about these files not being in the sources list.
  set(LLVM_OPTIONAL_SOURCES
    weak.mm
    Refcounting.mm
    Actor.cpp
    TaskStatus.cpp)

  add_swift_unittest(SwiftRuntimeTests
    Array.cpp
    CompatibilityOverrideRuntime.cpp
    CompatibilityOverrideConcurrency.cpp
    Concurrent.cpp
    Metadata.cpp
    Enum.cpp
    ExtendedExistential.cpp
    Refcounting.cpp
    Stdlib.cpp
    StackAllocator.cpp
    ${PLATFORM_SOURCES}

    # The runtime tests link to internal runtime symbols, which aren't exported
    # from the swiftCore dylib, so we need to link to both the runtime archive
    # and the stdlib.
    $<TARGET_OBJECTS:swiftRuntime${SWIFT_PRIMARY_VARIANT_SUFFIX}>
    $<TARGET_OBJECTS:swiftLLVMSupport${SWIFT_PRIMARY_VARIANT_SUFFIX}>
    $<TARGET_OBJECTS:swiftDemangling${SWIFT_PRIMARY_VARIANT_SUFFIX}>
    )

  # The local stdlib implementation provides definitions of the swiftCore
  # interfaces to avoid pulling in swiftCore itself.  Build the SwiftRuntimeTests
  # with swiftCore_EXPORTS to permit exporting the stdlib interfaces.
  target_compile_definitions(SwiftRuntimeTests
                             PRIVATE
                               swiftCore_EXPORTS
                               SWIFT_INLINE_NAMESPACE=__runtime)

  if(SWIFT_STDLIB_SUPPORT_BACK_DEPLOYMENT)
    target_compile_definitions(SwiftRuntimeTests
                               PRIVATE SWIFT_STDLIB_SUPPORT_BACK_DEPLOYMENT)
  endif()

  target_include_directories(SwiftRuntimeTests BEFORE PRIVATE
    ${SWIFT_SOURCE_DIR}/stdlib/include)
  target_include_directories(SwiftRuntimeTests BEFORE PRIVATE
    ${SWIFT_SOURCE_DIR}/stdlib/public)

  # FIXME: cross-compile for all variants.
  target_link_libraries(SwiftRuntimeTests
    PRIVATE
    swiftCore${SWIFT_PRIMARY_VARIANT_SUFFIX}
    swiftThreading${SWIFT_PRIMARY_VARIANT_SUFFIX}
    ${PLATFORM_TARGET_LINK_LIBRARIES}
    )
endif()
