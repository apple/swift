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
  let data = FileManager.default.contents(atPath: path)!
  return String(decoding: data, as: UTF8.self)
}

public func write(_ data: String, to path: String) {
  FileManager.default.createFile(
    atPath: path,
    contents: data.data(using: .utf8)
  )
}
