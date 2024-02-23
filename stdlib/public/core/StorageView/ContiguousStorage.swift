//===--- ContiguousStorage.swift ------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2024 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

public protocol ContiguousStorage<Element>: ~Copyable, ~Escapable {
  associatedtype Element: ~Copyable & ~Escapable

  var storage: StorageView<Element> { borrowing get }
}

extension StorageView: ContiguousStorage where Element: ~Copyable & ~Escapable {
  public var storage: Self { self }
}

extension Array: ContiguousStorage {
  public var storage: StorageView<Element> {
    borrowing _read {
      if let a = _baseAddressIfContiguous {
        yield StorageView(
          unsafePointer: a, count: count, owner: self
        )
      }
      else {
        let a = ContiguousArray(copy self)
        yield a.storage
      }
    }
  }
}

extension ContiguousArray: ContiguousStorage {
  public var storage: StorageView<Element> {
    borrowing _read {
      yield StorageView(
        unsafePointer: _baseAddressIfContiguous!, count: count, owner: self
      )
    }
  }
}
