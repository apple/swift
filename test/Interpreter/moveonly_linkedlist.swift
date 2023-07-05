// RUN: %target-run-simple-swift(-Xfrontend -sil-verify-all)
// RUN: %target-run-simple-swift(-O -Xfrontend -sil-verify-all)

// REQUIRES: executable_test

/// A class that we use as a box to store the memory for one of our linked list
/// nodes. It is on purpose fileprivate since it is an implementation detail of
/// \p NodeBox.
fileprivate final class _Box<T> {
  var value: _Node<T>

  init(_ value: consuming _Node<T>) { self.value = value }
}

struct _Node<T> : ~Copyable {
  var value: T
  var _next: ListEntry<T> = ListEntry<T>()

  init(_ newValue: T) {
    value = newValue
  }
}

/// A noncopyable box that contains the memory for a linked list node. Can be
/// embedded within other noncopyable data structures to point at a Node data
/// structure.
///
/// Internally uses a class as the actual box.
struct ListEntry<T> : ~Copyable {
  private var innerBox: _Box<T>?

  init() { innerBox = nil }
  init(initialValue value: consuming T) {
    innerBox = _Box<T>(_Node(value))
  }

  mutating func push(value newValue: consuming T) {
    if innerBox == nil {
      // If we do not already have a head, just take on this value and return.
      innerBox = _Box<T>(_Node(newValue))
      return
    }

    // Otherwise, we need to create a new node and fix things up.
    var nodeEntry = ListEntry<T>(initialValue: newValue)
    nodeEntry.next = self
    self = nodeEntry
  }

  mutating func pop() -> T? {
    guard let innerBox = innerBox else {
      return nil
    }

    let result = innerBox.value.value
    func fixNext(_ lhs: inout ListEntry<T>, _ rhs: inout ListEntry<T>) {
      lhs = rhs
      rhs = ListEntry<T>()
    }
    fixNext(&self, &innerBox.value._next)
    return result
  }

  var hasNext: Bool {
    return innerBox != nil
  }
  var next: ListEntry<T> {
    _modify {
      yield &innerBox!.value._next
    }
    _read {
      yield innerBox!.value._next
    }
  }

  var value: T? {
    return innerBox?.value.value
  }
}

let target = "ogyfbssvlh"
let strings = [
  "nulbhqylps",
  "hpdovhuybl",
  "bjjvpakqbm",
  "rqyozjzkyz",
  "qpzghmdcag",
  "lqefxvulvn",
  "wtokfqarxm",
  "acdcrzxpdg",
  "bxgfacpjic",
  "acblrvoego",
  "msevhriohn",
  "bamfcnbqvx",
  "wimkkqhryd",
  "dounctqkiw",
  "zxmyxcabhq",
  "ljerkuhlgy",
  "cettadahue",
  "cuummvmwly",
  "kdebludzsh",
  "ogyfbssvlh",
  "lrowrxwufj",
  "rftifkqggr",
  "ktjeeeobca",
  "xqlbnswmjr",
  "zpuxfbtmip",
  "rljcxrvdgh",
  "twkgardobr",
  "zrogczpzem",
  "bkuzjugksg",
  "eqanimdywo"
]

func buildList() -> ListEntry<String> {
  var head = ListEntry<String>()

  for i in strings {
    head.push(value: i)
  }

  return head
}

func main() {
  var head = buildList()
  var count = 0

  var strCount = strings.count
  while let x = head.pop() {
      assert(x == strings[strCount - count - 1])
      count = count + 1
  }
}

main()
