// RUN: %target-swift-frontend %s -emit-sil \
// RUN:   -o /dev/null \
// RUN:   -verify \
// RUN:   -sil-verify-all \
// RUN:   -module-name test \
// RUN:   -enable-experimental-feature NonescapableTypes

// REQUIRES: asserts
// REQUIRES: swift_in_compiler

struct NCInt: ~Copyable {
  var value: Int

  init(_ value: Int) { self.value = value }
}

struct NEInt : ~Escapable {
  let value: Int

  init(borrowed: borrowing NCInt) -> dependsOn(borrowed) Self {
    self.value = borrowed.value
    return self
  }
}

extension NCInt {
  var neInt: NEInt {
    borrowing get {
      NEInt(borrowed: self)
    }
  }
}

func ncint_get_neint_mutable_local() {
  var ncInt = NCInt(731)
  do {
    // Begin read access ncInt
    var neInt = ncInt.neInt

    // Capture neInt in a nonescapable closure.
    assert(ncInt.value == neInt.value)
    // End read access ncInt

    // Begin read access ncInt
    // Fully reassign neInt
    neInt = ncInt.neInt
    // Destroy neInt
    // End read access ncInt
  }
  ncInt.value = 1
}
