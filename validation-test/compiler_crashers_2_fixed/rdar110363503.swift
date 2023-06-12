// RUN: %target-swift-frontend -emit-ir %s -disable-availability-checking

struct ZipCollection<each C: Collection> {
  let c: (repeat each C)
}

extension ZipCollection: Collection {
  struct Element {
    var elt: (repeat each C.Element)
  }

  struct Index {
    let i: (repeat each C.Index)
  }

  var startIndex: Index {
    Index(i: (repeat (each c).startIndex))
  }

  var endIndex: Index {
    Index(i: (repeat (each c).endIndex))
  }

  func index(after i: Index) -> Index {
    Index(i: (repeat (each c).index(after: each i.i)))
  }

  subscript(index: Index) -> Element {
    Element(elt: (repeat (each c)[each index.i]))
  }
}

extension ZipCollection.Index: Equatable {
  static func ==(lhs: Self, rhs: Self) -> Bool {
    var result = true
    repeat result = ((each lhs.i) == (each rhs.i)) && result
    return result
  }
}

extension ZipCollection.Index: Comparable {
  static func <(lhs: Self, rhs: Self) -> Bool {
    var result: Bool? = nil
    func check<T: Comparable>(_ x: T, _ y: T) {
      if result == nil {
        if x == y { return }
        if x < y { result = true }
        if x > y { result = false }
      }
    }
    repeat check(each lhs.i, each rhs.i)
    return result ?? false
  }
}
