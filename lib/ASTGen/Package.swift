// swift-tools-version: 5.9

// The CMake build system is the only one that's able to produce a working
// compiler. This Package.swift makes it easier to build and work with the
// swiftASTGen library within IDEs, but it's mainly there for editing---it
// won't create something that can be meaningfully executed. Most things with
// the new Swift parser are better implemented/tested within or on top of the
// swift-syntax package.

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

let swiftSourceDirectory = #filePath
  .split(separator: "/", omittingEmptySubsequences: false)
  .dropLast(3) // Remove 'lib', 'ASTGen', 'Package.swift'
  .joined(separator: "/")

let swiftSetttings: [SwiftSetting] = [
  .interoperabilityMode(.Cxx),
  .unsafeFlags([
    "-Xcc", "-DCOMPILED_WITH_SWIFT",
    "-Xcc", "-UIBOutlet", "-Xcc", "-UIBAction", "-Xcc", "-UIBInspectable",
    "-Xcc", "-I\(swiftSourceDirectory)/include",
    "-Xcc", "-I\(swiftSourceDirectory)/../llvm-project/llvm/include",
    "-Xcc", "-I\(swiftSourceDirectory)/../llvm-project/clang/include",
    "-Xcc", "-I\(swiftSourceDirectory)/../build/Default/swift/include",
    "-Xcc", "-I\(swiftSourceDirectory)/../build/Default/llvm/include",
    "-Xcc", "-I\(swiftSourceDirectory)/../build/Default/llvm/tools/clang/include",

    // FIXME: Needed to work around an availability issue with CxxStdlib
    "-Xfrontend", "-disable-target-os-checking",
  ]),
]

let package = Package(
  name: "swiftSwiftCompiler",
  platforms: [
    .macOS(.v10_15)
  ],
  products: [
    .library(name: "swiftASTGen", targets: ["swiftASTGen"]),
  ],
  dependencies: [
    .package(path: "../../../swift-syntax")
  ],
  targets: [
    .target(
      name: "swiftASTGen",
      dependencies: [
        .product(name: "_SwiftCompilerPluginMessageHandling", package: "swift-syntax"),
        .product(name: "SwiftDiagnostics", package: "swift-syntax"),
        .product(name: "SwiftOperators", package: "swift-syntax"),
        .product(name: "SwiftParser", package: "swift-syntax"),
        .product(name: "SwiftParserDiagnostics", package: "swift-syntax"),
        .product(name: "SwiftSyntax", package: "swift-syntax"),
        .product(name: "SwiftSyntaxBuilder", package: "swift-syntax"),
        .product(name: "SwiftSyntaxMacroExpansion", package: "swift-syntax"),
        "_CompilerRegexParser",
      ],
      path: "Sources/ASTGen",
      swiftSettings: swiftSetttings
    ),
    .target(
      name: "swiftIDEUtilsBridging",
      dependencies: [
        "swiftASTGen",
        .product(name: "SwiftIDEUtils", package: "swift-syntax"),
        .product(name: "SwiftSyntax", package: "swift-syntax"),
      ],
      path: "Sources/SwiftIDEUtilsBridging",
      swiftSettings: swiftSetttings
    ),
    .target(
      name: "_CompilerRegexParser",
      dependencies: [],
      path: "_RegexParser_Sources",
      swiftSettings: swiftSetttings
    ),
  ],
  cxxLanguageStandard: .cxx17
)
