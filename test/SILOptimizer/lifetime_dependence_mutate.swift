// RUN: %target-swift-frontend %s -emit-sil \
// RUN:   -o /dev/null \
// RUN:   -verify \
// RUN:   -sil-verify-all \
// RUN:   -module-name test \
// RUN:   -enable-experimental-feature NoncopyableGenerics \
// RUN:   -enable-experimental-feature NonescapableTypes

// REQUIRES: asserts
// REQUIRES: swift_in_compiler

struct MBV : ~Escapable, ~Copyable {
  let p: UnsafeMutableRawPointer
  let c: Int

  @_unsafeNonescapableResult
  init(_ p: UnsafeMutableRawPointer, _ c: Int) {
    self.p = p
    self.c = c
  }

  subscript(position: Int) -> Int {
    get {
      let offset = position * MemoryLayout<Int>.stride
      return p.loadUnaligned(fromByteOffset: offset, as: Int.self)
    }
    nonmutating set(newValue) {
      let offset = position * MemoryLayout<Int>.stride
      p.storeBytes(of: newValue, toByteOffset: offset,
                   as: Int.self)
    }
  }
}

struct NC : ~Copyable {
  let p: UnsafeMutableRawPointer
  let c: Int

  // Requires a mutable borrow.
  mutating func getMBV() -> _mutate(self) MBV {
    MBV(p, c)
  }
}

func mbv_set_element(nc: inout NC, e: Int) {
  nc.getMBV()[3] = e
}
