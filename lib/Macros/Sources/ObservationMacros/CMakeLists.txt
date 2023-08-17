#===--- CMakeLists.txt - Observation macros library ----------------------===#
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

add_swift_macro_library(ObservationMacros
  Availability.swift
  Extensions.swift
  ObservableMacro.swift
  SWIFT_DEPENDENCIES
    SwiftSyntax::SwiftDiagnostics
    SwiftSyntax::SwiftOperators
    SwiftSyntax::SwiftSyntaxBuilder
    SwiftSyntax::SwiftSyntax
    SwiftSyntax::SwiftSyntaxMacros
)
