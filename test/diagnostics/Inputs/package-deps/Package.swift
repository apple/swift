// swift-tools-version: 5.5
import PackageDescription

let package = Package(
    name: "MyPackage",
    dependencies: [
        .package(url: "https://github.com/apple/swift-testing.git", from: "0.8.0"),
    ],
    targets: [
        .testTarget(
            name: "MyTest"
        ),
    ]
)

// expected-note@-6{{add dependency on product 'Testing' from the package 'swift-testing' to the package manifest}}
// expected-note@-7{{add dependency on product 'Testing' from the package 'swift-testing' to the package manifest}}{{9:15-12:10=\n        .testTarget(\n            name: "MyTest",\n            dependencies: [\n                .product(name: "Testing", package: "swift-testing"),\n            ]\n        )}}
