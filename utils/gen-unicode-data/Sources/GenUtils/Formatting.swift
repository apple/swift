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

// Given a collection, format it into a string within 80 columns and fitting as
// many elements in a row as possible.
public func formatCollection<C: Collection>(
  _ c: C,
  into result: inout String,
  using handler: (C.Element) -> String
) {
  // Our row length always starts at 2 for the initial indentation.
  var rowLength = 2

  for element in c {
    let string = handler(element)

    if rowLength == 2 {
      result += "  "
    }

    if rowLength + string.count + 1 > 80 {
      result += "\n  "

      rowLength = 2
    } else {
      result += rowLength == 2 ? "" : " "
    }

    result += "\(string),"

    // string.count + , + space
    rowLength += string.count + 1 + 1
  }
}
