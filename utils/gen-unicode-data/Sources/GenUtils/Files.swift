//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2021 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

import Foundation

public func readFile(_ path: String) -> String {
  do {
    return try String(contentsOfFile: path, encoding: .utf8)
  } catch {
    fatalError(error.localizedDescription)
  }
}

public func write(_ data: String, to path: String) {
  do {
    let directory = path.prefix(upTo: path.lastIndex(of: "/") ?? path.endIndex)
    try FileManager.default.createDirectory(
      atPath: String(directory),
      withIntermediateDirectories: true
    )
    try data.write(
      toFile: path,
      atomically: false,
      encoding: .utf8
    )
  } catch {
    fatalError(error.localizedDescription)
  }
}
