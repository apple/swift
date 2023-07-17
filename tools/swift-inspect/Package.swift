// swift-tools-version:5.3
// The swift-tools-version declares the minimum version of Swift required to build this package.

import PackageDescription

let package = Package(
    name: "swift-inspect",
    products: [
        .library(name: "SwiftInspectClient", type: .dynamic, targets: ["SwiftInspectClient"]),
    ],
    dependencies: [
        .package(url: "https://github.com/apple/swift-argument-parser", from: "0.0.1"),
    ],
    targets: [
        // Targets are the basic building blocks of a package. A target can define a module or a test suite.
        // Targets can depend on other targets in this package, and on products in packages which this package depends on.
        .target(
            name: "swift-inspect",
            dependencies: [
                "SymbolicationShims",
                .product(name: "ArgumentParser", package: "swift-argument-parser"),
                .target(name: "SwiftInspectClient", condition: .when(platforms: [.windows])),
                .target(name: "SwiftInspectClientInterface", condition: .when(platforms: [.windows])),
            ],
            swiftSettings: [
                .unsafeFlags([
                    "-parse-as-library",
                ]),
            ]),
        .target(
            name: "SwiftInspectClient"),
        .systemLibrary(
            name: "SwiftInspectClientInterface"),
        .testTarget(
            name: "swiftInspectTests",
            dependencies: ["swift-inspect"]),
        .systemLibrary(
            name: "SymbolicationShims")
    ]
)
