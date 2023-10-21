// swift-tools-version:5.9
//===--- Package.swift.in - SwiftCompiler SwiftPM package -----------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2021 - 2022 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

// To successfully build, you'll need to create a couple of symlinks to an
// existing Ninja build:
//
// ln -s <project-root>/build/<Ninja-Build>/llvm-<os+arch> <project-root>/build/Default/llvm
// ln -s <project-root>/build/<Ninja-Build>/swift-<os+arch> <project-root>/build/Default/swift
//
// where <project-root> is the parent directory of the swift repository.
//
// FIXME: We may want to consider generating Package.swift as a part of the
// build.

import PackageDescription

private extension Target {
  static func compilerModuleTarget(
    name: String,
    dependencies: [Dependency],
    path: String? = nil,
    sources: [String]? = nil,
    swiftSettings: [SwiftSetting] = []) -> Target {
      .target(
        name: name,
        dependencies: dependencies,
        path: path ?? "Sources/\(name)",
        exclude: ["CMakeLists.txt"],
        sources: sources,
        swiftSettings: [
          .interoperabilityMode(.Cxx),
          .unsafeFlags([
            "-static",
            "-Xcc", "-UIBOutlet", "-Xcc", "-UIBAction", "-Xcc", "-UIBInspectable",
            "-Xcc", "-I../include",
            "-Xcc", "-I../../llvm-project/llvm/include",
            "-Xcc", "-I../../llvm-project/clang/include",
            "-Xcc", "-I../../build/Default/swift/include",
            "-Xcc", "-I../../build/Default/llvm/include",
            "-Xcc", "-I../../build/Default/llvm/tools/clang/include",
            "-cross-module-optimization",
          ]),
        ] + swiftSettings)
    }
}

let package = Package(
  name: "SwiftCompilerSources",
  platforms: [
    .macOS(.v13),
  ],
  products: [
    .library(
      name: "swiftCompilerModules",
      type: .static,
      targets: ["Basic", "AST", "Parse", "SIL", "Optimizer", "_CompilerRegexParser"]),
  ],
  dependencies: [
  ],
  // Note that targets and their dependencies must align with
  // 'SwiftCompilerSources/Sources/CMakeLists.txt'
  targets: [
    .compilerModuleTarget(
      name: "_CompilerRegexParser",
      dependencies: [],
      path: "_RegexParser_Sources",
      swiftSettings: [
        // Workaround until `_CompilerRegexParser` is imported as implementation-only
        // by `_StringProcessing`.
        .unsafeFlags([
          "-Xfrontend",
          "-disable-implicit-string-processing-module-import"
        ])]),
    .compilerModuleTarget(
      name: "Basic",
      dependencies: []),
    .compilerModuleTarget(
      name: "AST",
      dependencies: ["Basic"]),
    .compilerModuleTarget(
      name: "Parse",
      dependencies: ["Basic", "AST", "_CompilerRegexParser"]),
    .compilerModuleTarget(
      name: "SIL",
      dependencies: ["Basic"]),
    .compilerModuleTarget(
      name: "Optimizer",
      dependencies: ["Basic", "SIL", "Parse"]),
  ],
  cxxLanguageStandard: .cxx17
)
