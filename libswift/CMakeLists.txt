# This source file is part of the Swift.org open source project
#
# Copyright (c) 2014 - 2021 Apple Inc. and the Swift project authors
# Licensed under Apache License v2.0 with Runtime Library Exception
#
# See http://swift.org/LICENSE.txt for license information
# See http://swift.org/CONTRIBUTORS.txt for Swift project authors

if (NOT SWIFT_TOOLS_ENABLE_LIBSWIFT)
  # A dummy libswift if libswift is disabled
  add_swift_host_library(libswift OBJECT LibSwiftStubs.cpp)
else()
  project(LibSwift LANGUAGES C CXX Swift)

  if (NOT CMAKE_Swift_COMPILER)
    message(FATAL_ERROR "Need a swift toolchain for building libswift")
  endif()

  add_subdirectory(Sources)

  add_libswift("libswift")  
endif()

