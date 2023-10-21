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

let swiftSetttings: [SwiftSetting] = [
  .interoperabilityMode(.Cxx),
  .unsafeFlags([
    "-Xfrontend", "-debug-cycles",
    "-Xcc", "-DIMPORTING_INTO_SWIFT", "-DIMPORTING_INTO_ASTGEN",
    "-Xcc", "-UIBOutlet", "-Xcc", "-UIBAction", "-Xcc", "-UIBInspectable",
    "-Xcc", "-I../../include",
    "-Xcc", "-I../../../llvm-project/llvm/include",
    "-Xcc", "-I../../../llvm-project/clang/include",
    "-Xcc", "-I../../../build/Default/swift/include",
    "-Xcc", "-I../../../build/Default/llvm/include",
    "-Xcc", "-I../../../build/Default/llvm/tools/clang/include",
  ])
]

let package = Package(
  name: "swiftSwiftCompiler",
  platforms: [
    .macOS(.v13)
  ],
  products: [
    .library(name: "swiftASTGen", targets: ["swiftASTGen"]),
    .library(name: "swiftLLVMJSON", targets: ["swiftLLVMJSON"]),
  ],
  dependencies: [
    .package(path: "../../../swift-syntax")
  ],
  targets: [
    .target(
      name: "swiftASTGen",
      dependencies: [
        .product(name: "SwiftBasicFormat", package: "swift-syntax"),
        .product(name: "SwiftCompilerPluginMessageHandling", package: "swift-syntax"),
        .product(name: "SwiftDiagnostics", package: "swift-syntax"),
        .product(name: "SwiftOperators", package: "swift-syntax"),
        .product(name: "SwiftParser", package: "swift-syntax"),
        .product(name: "SwiftParserDiagnostics", package: "swift-syntax"),
        .product(name: "SwiftSyntax", package: "swift-syntax"),
        .product(name: "SwiftSyntaxBuilder", package: "swift-syntax"),
        .product(name: "SwiftSyntaxMacros", package: "swift-syntax"),
        .product(name: "SwiftSyntaxMacroExpansion", package: "swift-syntax"),
        "swiftLLVMJSON"
      ],
      path: "Sources/ASTGen",
      swiftSettings: swiftSetttings
    ),
    .target(
      name: "swiftLLVMJSON",
      dependencies: [],
      path: "Sources/LLVMJSON",
      swiftSettings: swiftSetttings
    ),
  ],
  cxxLanguageStandard: .cxx17
)
