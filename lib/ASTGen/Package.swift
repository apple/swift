// swift-tools-version: 5.6

// The CMake build system is the only one that's able to produce a working
// compiler. This Package.swift makes it easier to build and work with the
// swiftASTGen library within IDEs, but it's mainly there for editing---it
// won't create something that can be meaningfully executed. Most things with
// the new Swift parser are better implemented/tested within or on top of the
// swift-syntax package.

import PackageDescription

let swiftSetttings: [SwiftSetting] = [
  .unsafeFlags([
    "-I", "../../include/swift/",
    "-I", "../../include",
  ])
]

let package = Package(
  name: "swiftSwiftCompiler",
  platforms: [
    .macOS(.v10_15)
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
        .product(name: "SwiftDiagnostics", package: "swift-syntax"),
        .product(name: "SwiftSyntax", package: "swift-syntax"),
        .product(name: "SwiftOperators", package: "swift-syntax"),
        .product(name: "SwiftParser", package: "swift-syntax"),
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
  ]
)
