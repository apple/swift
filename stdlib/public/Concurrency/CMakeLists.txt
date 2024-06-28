#===--- CMakeLists.txt - Concurrency support library ---------------------===#
#
# This source file is part of the Swift.org open source project
#
# Copyright (c) 2019 - 2020 Apple Inc. and the Swift project authors
# Licensed under Apache License v2.0 with Runtime Library Exception
#
# See https://swift.org/LICENSE.txt for license information
# See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
#
#===----------------------------------------------------------------------===#

set(SWIFT_RUNTIME_CONCURRENCY_C_FLAGS)
set(SWIFT_RUNTIME_CONCURRENCY_SWIFT_FLAGS -I${CMAKE_CURRENT_SOURCE_DIR}/InternalShims)

set(swift_concurrency_private_link_libraries)
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
  list(APPEND swift_concurrency_private_link_libraries Synchronization)
endif()

set(swift_concurrency_incorporate_object_libraries_so swiftThreading)

if("${SWIFT_CONCURRENCY_GLOBAL_EXECUTOR}" STREQUAL "dispatch")
  if(NOT CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    include_directories(AFTER
                          ${SWIFT_PATH_TO_LIBDISPATCH_SOURCE})

    # FIXME: we can't rely on libdispatch having been built for the
    # target at this point in the process.  Currently, we're relying
    # on soft-linking.
    list(APPEND swift_concurrency_link_libraries
      dispatch)
  endif()
elseif("${SWIFT_CONCURRENCY_GLOBAL_EXECUTOR}" STREQUAL "singlethreaded" OR
       "${SWIFT_CONCURRENCY_GLOBAL_EXECUTOR}" STREQUAL "hooked" OR
       "${SWIFT_CONCURRENCY_GLOBAL_EXECUTOR}" STREQUAL "none")
  list(APPEND SWIFT_RUNTIME_CONCURRENCY_C_FLAGS
    "-DSWIFT_CONCURRENCY_ENABLE_DISPATCH=0")
else()
  message(FATAL_ERROR "Invalid value for SWIFT_CONCURRENCY_GLOBAL_EXECUTOR (\"${SWIFT_CONCURRENCY_GLOBAL_EXECUTOR}\").")
endif()

if(NOT SWIFT_CONCURRENCY_USES_DISPATCH)

endif()

if(NOT swift_concurrency_async_fp_mode)
  set(swift_concurrency_async_fp_mode "always")
endif()

# Don't emit extended frame info on platforms other than darwin, system
# backtracer and system debugger are unlikely to support it.
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  list(APPEND SWIFT_RUNTIME_CONCURRENCY_C_FLAGS
    "-fswift-async-fp=${swift_concurrency_async_fp_mode}")
  list(APPEND SWIFT_RUNTIME_CONCURRENCY_SWIFT_FLAGS
    "-Xfrontend"
    "-swift-async-frame-pointer=${swift_concurrency_async_fp_mode}")
else()
  list(APPEND SWIFT_RUNTIME_CONCURRENCY_C_FLAGS "-fswift-async-fp=never")
endif()

list(APPEND SWIFT_RUNTIME_CONCURRENCY_C_FLAGS
  "-D__STDC_WANT_LIB_EXT1__=1")

set(SWIFT_RUNTIME_CONCURRENCY_C_SOURCES
  ../CompatibilityOverride/CompatibilityOverride.cpp
  Actor.cpp
  AsyncLet.cpp
  Clock.cpp
  GlobalExecutor.cpp
  EmbeddedSupport.cpp
  Error.cpp
  Setup.cpp
  Task.cpp
  TaskAlloc.cpp
  TaskStatus.cpp
  TaskGroup.cpp
  TaskLocal.cpp
  ThreadingError.cpp
  TracingSignpost.cpp
  AsyncStream.cpp
  linker-support/magic-symbols-for-install-name.c
)

set(SWIFT_RUNTIME_CONCURRENCY_SWIFT_SOURCES
  Actor.swift
  AsyncLet.swift
  CheckedContinuation.swift
  Errors.swift
  Executor.swift
  ExecutorAssertions.swift
  AsyncCompactMapSequence.swift
  AsyncDropFirstSequence.swift
  AsyncDropWhileSequence.swift
  AsyncFilterSequence.swift
  AsyncFlatMapSequence.swift
  AsyncIteratorProtocol.swift
  AsyncMapSequence.swift
  AsyncPrefixSequence.swift
  AsyncPrefixWhileSequence.swift
  AsyncSequence.swift
  AsyncThrowingCompactMapSequence.swift
  AsyncThrowingDropWhileSequence.swift
  AsyncThrowingFilterSequence.swift
  AsyncThrowingFlatMapSequence.swift
  AsyncThrowingMapSequence.swift
  AsyncThrowingPrefixWhileSequence.swift
  GlobalActor.swift
  GlobalConcurrentExecutor.swift
  MainActor.swift
  PartialAsyncTask.swift
  SourceCompatibilityShims.swift
  Task.swift
  Task+TaskExecutor.swift
  TaskCancellation.swift
  TaskGroup.swift
  TaskGroup+TaskExecutor.swift
  DiscardingTaskGroup.swift
  TaskLocal.swift
  TaskSleep.swift
  AsyncStreamBuffer.swift
  AsyncStream.swift
  AsyncThrowingStream.swift
  Deque/_DequeBuffer.swift
  Deque/_DequeBufferHeader.swift
  Deque/_DequeSlot.swift
  Deque/_UnsafeWrappedBuffer.swift
  Deque/Compatibility.swift
  Deque/Deque+Storage.swift
  Deque/Deque+UnsafeHandle.swift
  Deque/Deque.swift
  Deque/Deque+Codable.swift
  Deque/Deque+Collection.swift
  Deque/Deque+CustomDebugStringConvertible.swift
  Deque/Deque+CustomReflectable.swift
  Deque/Deque+CustomStringConvertible.swift
  Deque/Deque+Equatable.swift
  Deque/Deque+ExpressibleByArrayLiteral.swift
  Deque/Deque+Extras.swift
  Deque/Deque+Hashable.swift
  Deque/Deque+Testing.swift
  Deque/UnsafeMutableBufferPointer+Utilities.swift
  Clock.swift
  ContinuousClock.swift
  SuspendingClock.swift
  TaskSleepDuration.swift
)

add_swift_target_library(swift_Concurrency ${SWIFT_STDLIB_LIBRARY_BUILD_TYPES} IS_STDLIB
  ${SWIFT_RUNTIME_CONCURRENCY_C_SOURCES}
  ${SWIFT_RUNTIME_CONCURRENCY_SWIFT_SOURCES}

  SWIFT_MODULE_DEPENDS_ANDROID Android
  SWIFT_MODULE_DEPENDS_LINUX Glibc
  SWIFT_MODULE_DEPENDS_FREEBSD Glibc
  SWIFT_MODULE_DEPENDS_OPENBSD Glibc
  SWIFT_MODULE_DEPENDS_CYGWIN Glibc
  SWIFT_MODULE_DEPENDS_HAIKU Glibc
  SWIFT_MODULE_DEPENDS_WINDOWS CRT

  PRIVATE_LINK_LIBRARIES ${swift_concurrency_private_link_libraries}
  INCORPORATE_OBJECT_LIBRARIES_SHARED_ONLY
    ${swift_concurrency_incorporate_object_libraries_so}
  LINK_LIBRARIES ${swift_concurrency_link_libraries}

  C_COMPILE_FLAGS
    -Dswift_Concurrency_EXPORTS ${SWIFT_RUNTIME_CONCURRENCY_C_FLAGS}
    -I${SWIFT_SOURCE_DIR}/stdlib/include
  SWIFT_COMPILE_FLAGS
    ${SWIFT_STANDARD_LIBRARY_SWIFT_FLAGS}
    -parse-stdlib
    ${SWIFT_RUNTIME_CONCURRENCY_SWIFT_FLAGS}
  ${swift_concurrency_options}
  LINK_FLAGS "${SWIFT_RUNTIME_SWIFT_LINK_FLAGS}"
  INSTALL_IN_COMPONENT stdlib
  MACCATALYST_BUILD_FLAVOR zippered
)

# Embedded Swift Concurrency library
set(SWIFT_SHOULD_BUILD_EMBEDDED_CONCURRENCY TRUE)
is_build_type_optimized("${SWIFT_STDLIB_BUILD_TYPE}" swift_optimized)
if(NOT swift_optimized)
  set(SWIFT_SHOULD_BUILD_EMBEDDED_CONCURRENCY FALSE)
elseif(BOOTSTRAPPING_MODE STREQUAL "OFF")
  set(SWIFT_SHOULD_BUILD_EMBEDDED_CONCURRENCY FALSE)
elseif(SWIFT_HOST_VARIANT STREQUAL "windows") # For now, don't build embedded Concurrency on Windows
  set(SWIFT_SHOULD_BUILD_EMBEDDED_CONCURRENCY FALSE)
endif()
if(SWIFT_SHOULD_BUILD_EMBEDDED_STDLIB AND SWIFT_SHOULD_BUILD_EMBEDDED_CONCURRENCY)
  add_custom_target(embedded-concurrency)
  add_dependencies(embedded-libraries embedded-concurrency)

  set(SWIFT_ENABLE_REFLECTION OFF)
  set(SWIFT_STDLIB_SUPPORT_BACK_DEPLOYMENT OFF)
  set(SWIFT_STDLIB_STABLE_ABI OFF)
  set(SWIFT_STDLIB_ENABLE_OBJC_INTEROP OFF)
  set(SWIFT_CONCURRENCY_USES_DISPATCH FALSE)
  set(SWIFT_STDLIB_SINGLE_THREADED_CONCURRENCY TRUE)
  set(SWIFT_STDLIB_CONCURRENCY_TRACING FALSE)
  set(SWIFT_STDLIB_HAS_ENVIRON FALSE)
  set(SWIFT_STDLIB_HAS_ASL FALSE)

  foreach(entry ${EMBEDDED_STDLIB_TARGET_TRIPLES})
    string(REGEX REPLACE "[ \t]+" ";" list "${entry}")
    list(GET list 0 arch)
    list(GET list 1 mod)
    list(GET list 2 triple)

    if (SWIFT_HOST_VARIANT STREQUAL "linux")
      if(NOT "${mod}" MATCHES "-linux-gnu$")
        continue()
      endif()
      set(extra_c_compile_flags)
      set(extra_swift_compile_flags)
    elseif (SWIFT_HOST_VARIANT STREQUAL "macosx")
      if(NOT "${mod}" MATCHES "-macos$")
        continue()
      endif()
      set(extra_c_compile_flags -D__MACH__ -D__APPLE__ -ffreestanding)
      set(extra_swift_compile_flags -Xcc -D__MACH__ -Xcc -D__APPLE__ -Xcc -ffreestanding)
    endif()
    
    set(SWIFT_SDK_embedded_THREADING_PACKAGE none)
    set(SWIFT_SDK_embedded_ARCH_${mod}_MODULE "${mod}")
    set(SWIFT_SDK_embedded_LIB_SUBDIR "embedded")
    set(SWIFT_SDK_embedded_ARCH_${mod}_TRIPLE "${triple}")

    add_swift_target_library_single(
      embedded-concurrency-${mod}
      swift_Concurrency
      STATIC
      IS_STDLIB IS_FRAGILE

      ${SWIFT_RUNTIME_CONCURRENCY_C_SOURCES}
      # TODO: Only a subset of Swift Concurrency .swift sources, for now.
      Actor.swift
      AsyncLet.swift
      CheckedContinuation.swift
      Errors.swift
      Executor.swift
      ExecutorAssertions.swift
      AsyncCompactMapSequence.swift
      AsyncDropFirstSequence.swift
      AsyncDropWhileSequence.swift
      AsyncFilterSequence.swift
      AsyncFlatMapSequence.swift
      AsyncIteratorProtocol.swift
      AsyncMapSequence.swift
      AsyncPrefixSequence.swift
      AsyncPrefixWhileSequence.swift
      AsyncSequence.swift
      AsyncThrowingCompactMapSequence.swift
      AsyncThrowingDropWhileSequence.swift
      AsyncThrowingFilterSequence.swift
      AsyncThrowingFlatMapSequence.swift
      AsyncThrowingMapSequence.swift
      AsyncThrowingPrefixWhileSequence.swift
      GlobalActor.swift
      PartialAsyncTask.swift
      Task.swift
      TaskCancellation.swift

      SWIFT_COMPILE_FLAGS
        ${extra_swift_compile_flags} -enable-experimental-feature Embedded
        -parse-stdlib -DSWIFT_CONCURRENCY_EMBEDDED
        ${SWIFT_RUNTIME_CONCURRENCY_SWIFT_FLAGS}
      C_COMPILE_FLAGS
        ${extra_c_compile_flags} ${SWIFT_RUNTIME_CONCURRENCY_C_FLAGS} -DSWIFT_CONCURRENCY_EMBEDDED=1
      MODULE_DIR "${CMAKE_BINARY_DIR}/lib/swift/embedded"
      SDK "embedded"
      ARCHITECTURE "${mod}"
      DEPENDS embedded-stdlib-${mod}
      INSTALL_IN_COMPONENT stdlib
      )
    swift_install_in_component(
      TARGETS embedded-concurrency-${mod}
      DESTINATION "lib/swift/embedded/${mod}"
      COMPONENT "stdlib"
      )
    swift_install_in_component(
      FILES "${SWIFTLIB_DIR}/embedded/${mod}/libswift_Concurrency.a"
      DESTINATION "lib/swift/embedded/${mod}/"
      COMPONENT "stdlib"
      PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE
      )
    set_property(TARGET embedded-concurrency-${mod} PROPERTY OSX_ARCHITECTURES "${arch}")

    add_dependencies(embedded-concurrency embedded-concurrency-${mod})
  endforeach()
endif()
