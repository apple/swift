// RUN: %target-swift-frontend -O -enforce-exclusivity=checked -emit-sil  -primary-file %s | %FileCheck %s --check-prefix=TESTSIL
// REQUIRES: optimized_stdlib,asserts
// REQUIRES: PTRSIZE=64

public var check: UInt64 = 0

@inline(never)
func sum(_ x: UInt64, _ y: UInt64) -> UInt64 {
  return x &+ y
}

// FIXME: The optimization should be able to merge these accesses, but
// it must first prove that no other conflicting read accesses occur
// within the existing read access scopes.
//
// TESTSIL-LABEL: sil [noinline] @$s17merge_exclusivity10MergeTest1yySiF : $@convention(thin)
// TESTSIL: bb0
// TESTSIL: [[GLOBALVAR:%.*]] = global_addr @$s17merge_exclusivity5checks6UInt64Vvp
// TESTSIL: [[B1:%.*]] = begin_access [modify] [dynamic] [no_nested_conflict] [[GLOBALVAR]]
// TESTSIL: end_access [[B1]]
// TESTSIL: bb4{{.*}}:
// TESTSIL: [[B2a:%.*]] = begin_access [read] [static] [no_nested_conflict] [[GLOBALVAR]]
// TESTSIL-NEXT: load [[B2a]]
// TESTSIL: end_access [[B2a]]
// TESTSIL: [[B2b:%.*]] = begin_access [modify] [static] [no_nested_conflict] [[GLOBALVAR]]
// TESTSIL: store {{.*}} to [[B2b]]
// TESTSIL: end_access [[B2b]]
// TESTSIL: bb5:
// TESTSIL: [[B3a:%.*]] = begin_access [read] [static] [no_nested_conflict] [[GLOBALVAR]]
// TESTSIL-NEXT: load [[B3a]]
// TESTSIL: end_access [[B3a]]
// TESTSIL: [[B3b:%.*]] = begin_access [modify] [static] [no_nested_conflict] [[GLOBALVAR]]
// TESTSIL: store {{.*}} to [[B3b]]
// TESTSIL: end_access [[B3b]]
// TESTSIL: bb6:
// TESTSIL: [[B4a:%.*]] = begin_access [read] [static] [no_nested_conflict] [[GLOBALVAR]]
// TESTSIL-NEXT: load [[B4a]]
// TESTSIL: end_access [[B4a]]
// TESTSIL: [[B4b:%.*]] = begin_access [modify] [static] [no_nested_conflict] [[GLOBALVAR]]
// TESTSIL: store {{.*}} to [[B4b]]
// TESTSIL: end_access [[B4b]]
// TESTSIL-NOT: begin_access
// TESTSIL-LABEL: } // end sil function '$s17merge_exclusivity10MergeTest1yySiF'
@inline(never)
public func MergeTest1(_ N: Int) {
  let range = 0..<10000
  check = 0
  for _ in 1...N {
    for e in range {
      check = sum(check, UInt64(e))
      if (e == 0) {
        check = sum(check, UInt64(1))
      }
      else {
        check = sum(check, UInt64(2))
      }
    }
  }
}

// FIXME: The optimization should be able to merge these accesses, but
// it must first prove that no other conflicting read accesses occur
// within the existing read access scopes.
//
// TESTSIL-LABEL: sil [noinline] @$s17merge_exclusivity10MergeTest2yySiF : $@convention(thin)
// TESTSIL: bb0
// TESTSIL: [[GLOBALVAR:%.*]] = global_addr @$s17merge_exclusivity5checks6UInt64Vvp
// TESTSIL: [[B1:%.*]] = begin_access [modify] [dynamic] [no_nested_conflict] [[GLOBALVAR]]
// TESTSIL: end_access [[B1]]
// TESTSIL: bb6
// TESTSIL: [[B2a:%.*]] = begin_access [read] [static] [no_nested_conflict] [[GLOBALVAR]]
// TESTSIL-NEXT: load [[B2a]]
// TESTSIL: end_access [[B2a]]
// TESTSIL: [[B2b:%.*]] = begin_access [modify] [static] [no_nested_conflict] [[GLOBALVAR]]
// TESTSIL: store {{.*}} to [[B2b]]
// TESTSIL: end_access [[B2b]]
// TESTSIL: bb7
// TESTSIL: [[B3a:%.*]] = begin_access [read] [static] [no_nested_conflict] [[GLOBALVAR]]
// TESTSIL-NEXT: load [[B3a]]
// TESTSIL: end_access [[B3a]]
// TESTSIL: [[B3b:%.*]] = begin_access [modify] [static] [no_nested_conflict] [[GLOBALVAR]]
// TESTSIL: store {{.*}} to [[B3b]]
// TESTSIL: end_access [[B3b]]
// TESTSIL-NOT: begin_access
// TESTSIL-LABEL: } // end sil function '$s17merge_exclusivity10MergeTest2yySiF'
@inline(never)
public func MergeTest2(_ N: Int) {
  let range = 0..<10000
  check = 0
  for _ in 1...N {
    for e in range {
      if (e == 0) {
        check = sum(check, UInt64(1))
      }
      else {
        check = sum(check, UInt64(2))
      }
    }
  }
}

// FIXME: The optimization should be able to merge these accesses, but
// it must first prove that no other conflicting read accesses occur
// within the existing read access scopes.
//
// FIXME_TESTSIL-LABEL: sil [noinline] @$s17merge_exclusivity10MergeTest3yySiF : $@convention(thin)
// FIXME_TESTSIL: bb0
// FIXME_TESTSIL: [[GLOBALVAR:%.*]] = global_addr @$s17merge_exclusivity5checks6UInt64Vvp
// FIXME_TESTSIL: [[B1:%.*]] = begin_access [modify] [dynamic] [no_nested_conflict] [[GLOBALVAR]]
// FIXME_TESTSIL: end_access [[B1]]
// FIXME_TESTSIL-NOT: begin_access
// FIXME_TESTSIL-LABEL: } // end sil function '$s17merge_exclusivity10MergeTest3yySiF'
@inline(never)
public func MergeTest3(_ N: Int) {
  let range = 0..<10000
  check = 0
  for _ in 1...N {
    for e in range {
      check = sum(check, UInt64(e))
    }
  }
}

// FIXME: The optimization should be able to merge these accesses, but
// it must first prove that no other conflicting read accesses occur
// within the existing read access scopes.
//
// FIXME_TESTSIL-LABEL: sil [noinline] @$s17merge_exclusivity10MergeTest4yySiF : $@convention(thin)
// FIXME_TESTSIL: bb0
// FIXME_TESTSIL: [[GLOBALVAR:%.*]] = global_addr @$s17merge_exclusivity5checks6UInt64Vvp
// FIXME_TESTSIL: [[B1:%.*]] = begin_access [modify] [dynamic] [no_nested_conflict] [[GLOBALVAR]]
// FIXME_TESTSIL: end_access [[B1]]
// FIXME_TESTSIL: bb7
// FIXME_TESTSIL: [[B2:%.*]] = begin_access [modify] [static] [no_nested_conflict] [[GLOBALVAR]]
// FIXME_TESTSIL-NEXT: load [[B2]]
// FIXME_TESTSIL: store {{.*}} to [[B2]]
// FIXME_TESTSIL: end_access [[B2]]
// FIXME_TESTSIL: bb8
// FIXME_TESTSIL: [[B3:%.*]] = begin_access [modify] [static] [no_nested_conflict] [[GLOBALVAR]]
// FIXME_TESTSIL-NEXT: load [[B3]]
// FIXME_TESTSIL: store {{.*}} to [[B3]]
// FIXME_TESTSIL: end_access [[B3]]
// FIXME_TESTSIL-NOT: begin_access
// FIXME_TESTSIL-LABEL: } // end sil function '$s17merge_exclusivity10MergeTest4yySiF'
@inline(never)
public func MergeTest4(_ N: Int) {
  let range = 0..<10000
  check = 0
  for _ in 1...N {
    for e in range {
      if (e == 0) {
        check = sum(check, UInt64(1))
      }
      check = sum(check, UInt64(e))
    }
  }
}

// FIXME: The optimization should be able to merge these accesses, but
// it must first prove that no other conflicting read accesses occur
// within the existing read access scopes.
//
// FIXME_TESTSIL-LABEL: sil [noinline] @$s17merge_exclusivity10MergeTest5yySiF : $@convention(thin)
// FIXME_TESTSIL: bb0
// FIXME_TESTSIL: [[GLOBALVAR:%.*]] = global_addr @$s17merge_exclusivity5checks6UInt64Vvp
// FIXME_TESTSIL: [[B1:%.*]] = begin_access [modify] [dynamic] [no_nested_conflict] [[GLOBALVAR]]
// FIXME_TESTSIL: end_access [[B1]]
// FIXME_TESTSIL: bb6
// FIXME_TESTSIL: [[B2:%.*]] = begin_access [modify] [static] [no_nested_conflict] [[GLOBALVAR]]
// FIXME_TESTSIL-NEXT: load [[B2]]
// FIXME_TESTSIL: store {{.*}} to [[B2]]
// FIXME_TESTSIL: end_access [[B2]]
// FIXME_TESTSIL: bb7
// FIXME_TESTSIL: [[B3:%.*]] = begin_access [modify] [static] [no_nested_conflict] [[GLOBALVAR]]
// FIXME_TESTSIL-NEXT: load [[B3]]
// FIXME_TESTSIL: store {{.*}} to [[B3]]
// FIXME_TESTSIL: end_access [[B3]]
// FIXME_TESTSIL: bb8
// FIXME_TESTSIL: [[B4:%.*]] = begin_access [modify] [static] [no_nested_conflict] [[GLOBALVAR]]
// FIXME_TESTSIL-NEXT: load [[B4]]
// FIXME_TESTSIL: store {{.*}} to [[B4]]
// FIXME_TESTSIL: end_access [[B4]]
// FIXME_TESTSIL-NOT: begin_access
// FIXME_TESTSIL-LABEL: } // end sil function '$s17merge_exclusivity10MergeTest5yySiF'
@inline(never)
public func MergeTest5(_ N: Int) {
  let range = 0..<10000
  check = 0
  for _ in 1...N {
    for e in range {
      if (e == 0) {
        check = sum(check, UInt64(1))
      }
      else {
        check = sum(check, UInt64(2))
      }
      check = sum(check, UInt64(e))
    }
  }
}

// FIXME: The optimization should be able to merge these accesses, but
// it must first prove that no other conflicting read accesses occur
// within the existing read access scopes.
//
// FIXME_TESTSIL-LABEL: sil [noinline] @$s17merge_exclusivity10MergeTest6yySiF : $@convention(thin)
// FIXME_TESTSIL: bb0
// FIXME_TESTSIL: [[GLOBALVAR:%.*]] = global_addr @$s17merge_exclusivity5checks6UInt64Vvp
// FIXME_TESTSIL: [[B1:%.*]] = begin_access [modify] [dynamic] [no_nested_conflict] [[GLOBALVAR]]
// FIXME_TESTSIL: end_access [[B1]]
// FIXME_TESTSIL-NOT: begin_access
// FIXME_TESTSIL-LABEL: } // end sil function '$s17merge_exclusivity10MergeTest6yySiF'
@inline(never)
public func MergeTest6(_ N: Int) {
  let range = 0..<10000
  check = 0
  for _ in 1...N {
    for e in range {
      check = sum(check, UInt64(e))
      for _ in range {
        check = sum(check, UInt64(e))
      }
      check = sum(check, UInt64(e))
    }
  }
}

@inline(never)
public func foo() {
}

// FIXME: The optimization should be able to merge these accesses, but
// it must first prove that no other conflicting read accesses occur
// within the existing read access scopes.
//
// FIXME_TESTSIL-LABEL: sil [noinline] @$s17merge_exclusivity10MergeTest7yySiF : $@convention(thin)
// FIXME_TESTSIL: bb0
// FIXME_TESTSIL: [[GLOBALVAR:%.*]] = global_addr @$s17merge_exclusivity5checks6UInt64Vvp
// FIXME_TESTSIL: [[B1:%.*]] = begin_access [modify] [dynamic] [no_nested_conflict] [[GLOBALVAR]]
// FIXME_TESTSIL: end_access [[B1]]
// FIXME_TESTSIL-NOT: begin_access
// FIXME_TESTSIL-LABEL: } // end sil function '$s17merge_exclusivity10MergeTest7yySiF'
@inline(never)
public func MergeTest7(_ N: Int) {
  let range = 0..<10000
  check = 0
  for _ in 1...N {
    for e in range {
      check = sum(check, UInt64(e))
      for _ in range {
        foo()
      }
      check = sum(check, UInt64(e))
    }
  }
}

// FIXME: The optimization should be able to merge these accesses, but
// it must first prove that no other conflicting read accesses occur
// within the existing read access scopes.
//
// FIXME_TESTSIL-LABEL: sil [noinline] @$s17merge_exclusivity10MergeTest8yySiF : $@convention(thin)
// FIXME_TESTSIL: bb0
// FIXME_TESTSIL: [[GLOBALVAR:%.*]] = global_addr @$s17merge_exclusivity5checks6UInt64Vvp
// FIXME_TESTSIL: [[B1:%.*]] = begin_access [modify] [dynamic] [no_nested_conflict] [[GLOBALVAR]]
// FIXME_TESTSIL: end_access [[B1]]
// FIXME_TESTSIL-NOT: begin_access
// FIXME_TESTSIL-LABEL: } // end sil function '$s17merge_exclusivity10MergeTest8yySiF'
@inline(never)
public func MergeTest8(_ N: Int) {
  let range = 0..<10000
  check = 0
  for _ in 1...N {
    for e in range {
      check = sum(check, UInt64(e))
      for _ in range {
        foo()
      }
      check = sum(check, UInt64(e))
    }
  }
  for _ in 1...N {
    for e in range {
      check = sum(check, UInt64(e))
      for _ in range {
        foo()
      }
      check = sum(check, UInt64(e))
    }
  }
}

// Large, test that tests the interaction between access merging,
// and the rest of the access optimizations.

public protocol WriteProt {
    func writeTo(_ stream: StreamClass)
}

public final class StreamClass {
    private var buffer: [UInt8]
    
    public init() {
        self.buffer = []
    }

    @inline(__always)
    public func write(_ byte: UInt8) {
        buffer.append(byte)
    }

    @inline(__always)
    public func write(_ value: WriteProt) {
        value.writeTo(self)
    }

    @inline(__always)
    public func writeEscaped(_ string: String) {
        writeEscaped(string: string.utf8)
    }
    
    @inline(__always)
    public func writeEscaped<T: Collection>(
        string sequence: T
    ) where T.Iterator.Element == UInt8 {
        for character in sequence {
            buffer.append(character)
            buffer.append(character)
        }
    }
}

@inline(__always)
public func toStream(_ stream: StreamClass, _ value: WriteProt) -> StreamClass {
    stream.write(value)
    return stream
}

extension UInt8: WriteProt {
    @inline(__always)
    public func writeTo(_ stream: StreamClass) {
        stream.write(self)
    }
}

public func asWriteProt(_ string: String) -> WriteProt {
        return EscapedString(value: string)
}

private struct EscapedString: WriteProt {
    let value: String
        
    @inline(__always)
    func writeTo(_ stream: StreamClass) {
        _ = toStream(stream, UInt8(ascii: "a"))
        stream.writeEscaped(value)
        _ = toStream(stream, UInt8(ascii: "a"))
    }
}

public func asWriteProt<T>(_ items: [T], transform: @escaping (T) -> String) -> WriteProt {
    return EscapedTransform(items: items, transform: transform)
}

private struct EscapedTransform<T>: WriteProt {
    let items: [T]
    let transform: (T) -> String

    @inline(__always)
    func writeTo(_ stream: StreamClass) {
        for (i, item) in items.enumerated() {
            if i != 0 { _ = toStream(stream, asWriteProt(transform(item))) }
            _ = toStream(stream, asWriteProt(transform(item)))
        }
    }
}

// TESTSIL-LABEL: sil [noinline] @$s17merge_exclusivity14run_MergeTest9yySiF : $@convention(thin)
// TESTSIL: [[REFADDR:%.*]] = ref_element_addr {{.*}} : $StreamClass, #StreamClass.buffer
// TESTSIL-NEXT: store {{.*}} to [[REFADDR]]
// TESTSIL: [[BCONF:%.*]] = begin_access [modify] [{{.*}}] [[REFADDR]]
// TESTSIL: end_access [[BCONF]]
// TESTSIL: [[BCONF:%.*]] = begin_access [modify] [{{.*}}] [[REFADDR]]
// TESTSIL: end_access [[BCONF]]
// TESTSIL: [[BCONF:%.*]] = begin_access [modify] [{{.*}}] [[REFADDR]]
// TESTSIL: end_access [[BCONF]]
// TESTSIL-LABEL: } // end sil function '$s17merge_exclusivity14run_MergeTest9yySiF'
@inline(never)
public func run_MergeTest9(_ N: Int) {
  struct Thing {
    var value: String
    init(_ value: String) { self.value = value }
  }
  let listOfStrings: [String] = (0..<10).map { "This is the number: \($0)!\n" }
  let listOfThings: [Thing] = listOfStrings.map(Thing.init)
  for _ in 1...N {
    let stream = StreamClass()
    _ = toStream(stream, asWriteProt(listOfThings, transform: { $0.value }))
  }
}
