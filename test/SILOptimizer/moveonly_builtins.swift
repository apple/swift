// RUN: %target-swift-frontend-typecheck -verify %s -DILLEGAL \
// RUN:   -enable-builtin-module \
// RUN:   -verify-additional-prefix illegal-

// RUN: %target-swift-frontend -emit-sil -sil-verify-all -verify %s \
// RUN:   -enable-builtin-module

import Builtin

struct NC: ~Copyable {}

func checkPointerBuiltins(_ ptr: Builtin.RawPointer, _ value: consuming NC) {
  Builtin.initialize(value, ptr)
}

func checkArrayBuiltins(_ dest: Builtin.RawPointer, src: Builtin.RawPointer, count: Builtin.Word) {
  Builtin.takeArrayFrontToBack(NC.self, dest, src, count)
  Builtin.takeArrayBackToFront(NC.self, dest, src, count)
  Builtin.assignTakeArray(NC.self, dest, src, count)
  Builtin.destroyArray(NC.self, dest, count)

#if ILLEGAL
  Builtin.copyArray(NC.self, dest, src, count) // expected-illegal-error {{global function 'copyArray' requires that 'NC' conform to 'Copyable'}}
  Builtin.assignCopyArrayNoAlias(NC.self, dest, src, count) // expected-illegal-error {{global function 'assignCopyArrayNoAlias' requires that 'NC' conform to 'Copyable'}}
  Builtin.assignCopyArrayFrontToBack(NC.self, dest, src, count) // expected-illegal-error {{global function 'assignCopyArrayFrontToBack' requires that 'NC' conform to 'Copyable'}}
  Builtin.assignCopyArrayBackToFront(NC.self, dest, src, count) // expected-illegal-error {{global function 'assignCopyArrayBackToFront' requires that 'NC' conform to 'Copyable'}}
#endif
}

public func checkIllegal() {
#if ILLEGAL
  _ = Builtin.unsafeCastToNativeObject(NC())
  _ = Builtin.castToNativeObject(NC()) // expected-illegal-error {{global function 'castToNativeObject' requires that 'NC' conform to 'Copyable'}}
  let _: NC = Builtin.zeroInitializer()
#endif
}
