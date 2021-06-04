// swift-tools-version:5.3

import PackageDescription

let package = Package(
  name: "GenUnicodeData",
  targets: [
    .target(
      name: "GenUtils",
      dependencies: []
    ),
    .target(
      name: "GenGraphemeBreakProperty",
      dependencies: ["GenUtils"]
    )
  ]
)
