#===--- CMakeLists.txt - Swift macros library ----------------------===#
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

add_swift_macro_library(SwiftMacros
  OptionSetMacro.swift
  DebugDescriptionMacro.swift
  DistributedResolvableMacro.swift
  SyntaxExtensions.swift
  TaskLocalMacro.swift
  SWIFT_DEPENDENCIES
    SwiftDiagnostics
    SwiftSyntax
    SwiftSyntaxBuilder
    SwiftSyntaxMacros
)
