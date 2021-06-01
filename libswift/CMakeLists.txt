################################################################################
# This source file is part of the Swift.org open source project
#
# Copyright (c) 2014 - 2021 Apple Inc. and the Swift project authors
# Licensed under Apache License v2.0 with Runtime Library Exception
#
# See http://swift.org/LICENSE.txt for license information
# See http://swift.org/CONTRIBUTORS.txt for Swift project authors
################################################################################

# In the absence of fine grained tablegen dependencies we need to ensure that
# Swift's libraries all build after the LLVM & Clang tablegen-generated headers
# are generated. When building out-of-tree (as with build-script) LLVM & Clang's
# CMake configuration files create these targets as dummies so we can safely
# depend on them directly here (See: SR-6026)
# LLVM_COMMON_DEPENDS is a construct from the LLVM build system. It is a special
# purpose variable that provides common dependencies for all libraries, and
# executables generated when it is set. CMake's scoping rules enforce that these
# new dependencies will only be added to targets created under Swift's lib
# directory.
list(APPEND LLVM_COMMON_DEPENDS intrinsics_gen clang-tablegen-targets)

# Add generated libSyntax headers to global dependencies.
list(APPEND LLVM_COMMON_DEPENDS swift-syntax-generated-headers)
list(APPEND LLVM_COMMON_DEPENDS swift-parse-syntax-generated-headers)

if (NOT SWIFT_TOOLS_ENABLE_LIBSWIFT)
  # A dummy libswift if libswift is disabled
  add_swift_host_library(LibSwiftStub OBJECT LibSwiftStubs.cpp)
  add_swift_host_library(LibSwiftSIL STATIC $<TARGET_OBJECTS:LibSwiftStub>)
  add_swift_host_library(LibSwiftOptimizer STATIC $<TARGET_OBJECTS:LibSwiftStub>)
else()
  project(LibSwift LANGUAGES C CXX Swift)

  # Add path for custom CMake modules.
  list(APPEND CMAKE_MODULE_PATH
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/modules")

  add_subdirectory(Sources)
endif()
