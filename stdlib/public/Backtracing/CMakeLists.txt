#===--- CMakeLists.txt - Backtracing support library -----------------------===#
#
# This source file is part of the Swift.org open source project
#
# Copyright (c) 2023 Apple Inc. and the Swift project authors
# Licensed under Apache License v2.0 with Runtime Library Exception
#
# See https://swift.org/LICENSE.txt for license information
# See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
#
#===------------------------------------------------------------------------===#

set(swift_backtracing_link_libraries
  swiftCore
  swift_Concurrency
)

set(concurrency)
if(SWIFT_BUILD_STDLIB AND SWIFT_ENABLE_EXPERIMENTAL_CONCURRENCY)
  set(concurrency _Concurrency)
endif()

set(BACKTRACING_SOURCES
  ArrayImageSource.swift
  Backtrace.swift
  BacktraceFormatter.swift
  ByteSwapping.swift
  Context.swift
  Compression.swift
  CoreSymbolication.swift
  Dwarf.swift
  Elf.swift
  FileImageSource.swift
  FramePointerUnwinder.swift
  Image.swift
  ImageSource.swift
  MemoryImageSource.swift
  MemoryReader.swift
  ProcMapsScanner.swift
  Registers.swift
  SymbolicatedBacktrace.swift
  Utils.swift
  Win32Extras.cpp

  get-cpu-context.${SWIFT_ASM_EXT}
)

set(BACKTRACING_COMPILE_FLAGS
  "-Xfrontend;-experimental-spi-only-imports"
  "-Xcc;-I${SWIFT_SOURCE_DIR}/include"
  "-Xcc;-I${CMAKE_BINARY_DIR}/include"
  "-Xcc;-fno-implicit-module-maps"
  "-Xcc;-fbuiltin-module-map"
  "-Xcc;-fmodule-map-file=${SWIFT_STDLIB_SOURCE_DIR}/public/SwiftShims/swift/shims/module.modulemap"
  "-Xcc;-fmodule-map-file=${SWIFT_STDLIB_SOURCE_DIR}/public/Backtracing/modules/module.modulemap")

###TODO: Add these when we add static linking support
#
#list(APPEND BACKTRACING_COMPILE_FLAGS
#  "-Xcc;-I${SWIFT_PATH_TO_ZLIB_SOURCE}"
#  "-Xcc;-I${SWIFT_PATH_TO_ZSTD_SOURCE}/lib"
#  "-Xcc;-I${SWIFT_PATH_TO_LIBLZMA_SOURCE}/src/liblzma/api")

if(SWIFT_ASM_AVAILABLE)
  list(APPEND BACKTRACING_SOURCES get-cpu-context.${SWIFT_ASM_EXT})
  list(APPEND BACKTRACING_COMPILE_FLAGS "-DSWIFT_ASM_AVAILABLE")
else()
  message(warning "Assembly language not available on this platform; backtracing will fail.")
endif()

set(LLVM_OPTIONAL_SOURCES
  get-cpu-context.S
  get-cpu-context.asm
)

add_swift_target_library(swift_Backtracing ${SWIFT_STDLIB_LIBRARY_BUILD_TYPES} IS_STDLIB
  ${BACKTRACING_SOURCES}

  SWIFT_MODULE_DEPENDS ${concurrency}

  LINK_LIBRARIES ${swift_backtracing_link_libraries}

  SWIFT_COMPILE_FLAGS
    ${SWIFT_STANDARD_LIBRARY_SWIFT_FLAGS}
    ${BACKTRACING_COMPILE_FLAGS}
    -parse-stdlib

  LINK_FLAGS
    ${SWIFT_RUNTIME_SWIFT_LINK_FLAGS}

  INSTALL_IN_COMPONENT stdlib
  MACCATALYST_BUILD_FLAVOR "zippered"

  TARGET_SDKS OSX LINUX
)
