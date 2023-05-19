// RUN: rm -rf %t
// RUN: split-file %s %t
// RUN: %target-build-swift %t/test.swift -I %t/Inputs -Xfrontend -enable-experimental-cxx-interop -typecheck

//--- Inputs/module.modulemap
module Test {
  header "test.h"
  requires cplusplus
}

//--- Inputs/test.h

#define SWIFT_COMPUTED_PROPERTY \
__attribute__((swift_attr("import_computed_property")))

struct FirstRecordWithX {
  int getX() const SWIFT_COMPUTED_PROPERTY { return 42; }
};

struct SecondRecordWithXUsesFirst {
  int getX() const SWIFT_COMPUTED_PROPERTY { return 21; }

  const FirstRecordWithX * getY() const { return nullptr; }
};

//--- test.swift

import Test

func test(_ val: SecondRecordWithXUsesFirst) {
  let _ = val.x
}
