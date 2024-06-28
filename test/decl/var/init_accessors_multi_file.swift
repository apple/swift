// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend %s -verify -O -primary-file %s %S/Inputs/imported_init_accessor.swift -c -o %t/init_accessors_multi_file.o

extension S {
  init(extendedX: Int) {
    self.xImmutablePublic = extendedX
  }
}

func test() {
  let s = S(extendedX: 42)
  _ = s.xImmutablePublic
}
