#===--- CMakeLists.txt - Synchronization library ---------------------------===#
#
# This source file is part of the Swift.org open source project
#
# Copyright (c) 2023 Apple Inc. and the Swift project authors
# Licensed under Apache License v2.0 with Runtime Library Exception
#
# See https://swift.org/LICENSE.txt for license information
# See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
#
#===----------------------------------------------------------------------===#

set(SWIFT_SYNCHRONIZATION_ATOMIC_SOURCES
  Atomics/Atomic.swift
  Atomics/AtomicBool.swift
  Atomics/AtomicFloats.swift
  Atomics/AtomicLazyReference.swift
  Atomics/AtomicMemoryOrderings.swift
  Atomics/AtomicOptional.swift
  Atomics/AtomicPointers.swift
  Atomics/AtomicRepresentable.swift
  Atomics/WordPair.swift
)

set(SWIFT_SYNCHRONIZATION_SOURCES
  ${SWIFT_SYNCHRONIZATION_ATOMIC_SOURCES}

  Cell.swift
)

set(SWIFT_SYNCHRONIZATION_GYB_SOURCES
  Atomics/AtomicIntegers.swift.gyb
  Atomics/AtomicStorage.swift.gyb
)

# Darwin dependencies and sources

set(SWIFT_SYNCHRONIZATION_DARWIN_DEPENDENCIES)

if(SWIFT_BUILD_SDK_OVERLAY)
  set(SWIFT_SYNCHRONIZATION_DARWIN_DEPENDENCIES Darwin)
endif()

set(SWIFT_SYNCHRONIZATION_DARWIN_SOURCES
  Mutex/DarwinImpl.swift
  Mutex/Mutex.swift
)

# Linux and Android sources

set(SWIFT_SYNCHRONIZATION_LINUX_SOURCES
  Mutex/LinuxImpl.swift
  Mutex/Mutex.swift
  Mutex/SpinLoopHint.swift
)

# Wasm sources

set(SWIFT_SYNCHRONIZATION_WASM_SOURCES
  Mutex/Mutex.swift
  Mutex/WasmImpl.swift
)

# Windows sources

set(SWIFT_SYNCHRONIZATION_WINDOWS_SOURCES
  Mutex/Mutex.swift
  Mutex/WindowsImpl.swift
)

set(SWIFT_SYNCHRNOIZATION_SWIFT_FLAGS
  ${SWIFT_STANDARD_LIBRARY_SWIFT_FLAGS}
  "-enable-builtin-module"
  "-enable-experimental-feature" "RawLayout"
  "-enable-experimental-feature" "StaticExclusiveOnly"
  "-enable-experimental-feature" "Extern"
)

add_swift_target_library(swiftSynchronization ${SWIFT_STDLIB_LIBRARY_BUILD_TYPES} IS_STDLIB
  ${SWIFT_SYNCHRONIZATION_SOURCES}

  GYB_SOURCES
    ${SWIFT_SYNCHRONIZATION_GYB_SOURCES}

  SWIFT_SOURCES_DEPENDS_MACOS
    ${SWIFT_SYNCHRONIZATION_DARWIN_SOURCES}
  SWIFT_SOURCES_DEPENDS_IOS
    ${SWIFT_SYNCHRONIZATION_DARWIN_SOURCES}
  SWIFT_SOURCES_DEPENDS_TVOS
    ${SWIFT_SYNCHRONIZATION_DARWIN_SOURCES}
  SWIFT_SOURCES_DEPENDS_WATCHOS
    ${SWIFT_SYNCHRONIZATION_DARWIN_SOURCES}
  SWIFT_SOURCES_DEPENDS_VISIONOS
    ${SWIFT_SYNCHRONIZATION_DARWIN_SOURCES}
  SWIFT_SOURCES_DEPENDS_LINUX
    ${SWIFT_SYNCHRONIZATION_LINUX_SOURCES}
  SWIFT_SOURCES_DEPENDS_WASI
    ${SWIFT_SYNCHRONIZATION_WASM_SOURCES}
  SWIFT_SOURCES_DEPENDS_WINDOWS
    ${SWIFT_SYNCHRONIZATION_WINDOWS_SOURCES}
  SWIFT_SOURCES_DEPENDS_FREESTANDING
    Mutex/MutexUnavailable.swift

  SWIFT_MODULE_DEPENDS_OSX
    ${SWIFT_SYNCHRONIZATION_DARWIN_DEPENDENCIES}
  SWIFT_MODULE_DEPENDS_IOS
    ${SWIFT_SYNCHRONIZATION_DARWIN_DEPENDENCIES}
  SWIFT_MODULE_DEPENDS_TVOS
    ${SWIFT_SYNCHRONIZATION_DARWIN_DEPENDENCIES}
  SWIFT_MODULE_DEPENDS_WATCHOS
    ${SWIFT_SYNCHRONIZATION_DARWIN_DEPENDENCIES}
  SWIFT_MODULE_DEPENDS_XROS
    ${SWIFT_SYNCHRONIZATION_DARWIN_DEPENDENCIES}
  SWIFT_MODULE_DEPENDS_MACCATALYST
    ${SWIFT_SYNCHRONIZATION_DARWIN_DEPENDENCIES}
  SWIFT_MODULE_DEPENDS_LINUX
    Glibc
  SWIFT_MODULE_DEPENDS_ANDROID
    Android
  SWIFT_MODULE_DEPENDS_WINDOWS
    WinSDK

  SWIFT_COMPILE_FLAGS
    ${SWIFT_SYNCHRNOIZATION_SWIFT_FLAGS}
  LINK_FLAGS
    "${SWIFT_RUNTIME_SWIFT_LINK_FLAGS}"
  INSTALL_IN_COMPONENT
    stdlib
  MACCATALYST_BUILD_FLAVOR
    "zippered"
)

# Embedded Synchronization - embedded libraries are built as .swiftmodule only,
# i.e. there is no .o or .a file produced (no binary code is actually produced)
# and only users of a library are going to actually compile any needed code.
if(SWIFT_SHOULD_BUILD_EMBEDDED_STDLIB)
  add_custom_target(embedded-synchronization)
  add_dependencies(embedded-libraries embedded-synchronization)

  foreach(entry ${EMBEDDED_STDLIB_TARGET_TRIPLES})
    string(REGEX REPLACE "[ \t]+" ";" list "${entry}")
    list(GET list 0 arch)
    list(GET list 1 mod)
    list(GET list 2 triple)
    
    set(SWIFT_SDK_embedded_ARCH_${arch}_MODULE "${mod}")
    set(SWIFT_SDK_embedded_LIB_SUBDIR "embedded")
    set(SWIFT_SDK_embedded_ARCH_${arch}_TRIPLE "${triple}")
    set(SWIFT_SDK_embedded_PATH ${SWIFT_SDK_OSX_PATH})
    set(SWIFT_SDK_embedded_ARCH_${arch}_PATH ${SWIFT_SDK_OSX_PATH})
    set(SWIFT_SDK_embedded_USE_ISYSROOT TRUE)
    add_swift_target_library_single(
      embedded-synchronization-${mod}
      swiftSynchronization
      ONLY_SWIFTMODULE
      IS_FRAGILE
      
      ${SWIFT_SYNCHRONIZATION_ATOMIC_SOURCES}
      GYB_SOURCES
        ${SWIFT_SYNCHRONIZATION_GYB_SOURCES}

      SWIFT_COMPILE_FLAGS
        ${SWIFT_SYNCHRNOIZATION_SWIFT_FLAGS}
        -Xcc -D__MACH__ -Xcc -D__APPLE__ -Xcc -ffreestanding -enable-experimental-feature Embedded

      MODULE_DIR "${CMAKE_BINARY_DIR}/lib/swift/embedded"
      SDK "embedded"
      ARCHITECTURE "${arch}"
      DEPENDS embedded-stdlib-${mod}
      INSTALL_IN_COMPONENT stdlib
      )
    add_dependencies(embedded-synchronization embedded-synchronization-${mod})
  endforeach()
endif()
