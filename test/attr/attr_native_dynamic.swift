// RUN: %target-swift-frontend -swift-version 5 -typecheck -dump-ast %s | %FileCheck  %s

struct Strukt {
  // CHECK: (struct_decl {{.*}} "Strukt"
  // CHECK: (var_decl {{.*}} "dynamicStorageOnlyVar" type='Int' interface type='Int' access=internal dynamic readImpl=stored writeImpl=stored readWriteImpl=stored
  // CHECK: (accessor_decl {{.*}} get_for="dynamicStorageOnlyVar" {{.*}} access=internal dynamic{{$}}
  // CHECK: (accessor_decl {{.*}} set_for="dynamicStorageOnlyVar" {{.*}} access=internal dynamic{{$}}
  // CHECK: (accessor_decl {{.*}} _modify_for="dynamicStorageOnlyVar" {{.*}} access=internal{{$}}
  dynamic var dynamicStorageOnlyVar : Int = 0

  // CHECK: (var_decl {{.*}} "computedVar" type='Int' interface type='Int' access=internal dynamic readImpl=getter immutable
  // CHECK: (accessor_decl {{.*}} get_for="computedVar" {{.*}} access=internal dynamic{{$}}
  dynamic var computedVar : Int {
    return 0
  }

  // CHECK: (var_decl {{.*}} "computedVar2" type='Int' interface type='Int' access=internal dynamic readImpl=getter immutable
  // CHECK: (accessor_decl {{.*}} get_for="computedVar2" {{.*}} access=internal dynamic{{$}}
  dynamic var computedVar2 : Int {
    get {
      return 0
    }
  }

  // CHECK: (var_decl {{.*}} "computedVarGetterSetter" type='Int' interface type='Int' access=internal dynamic readImpl=getter writeImpl=setter readWriteImpl=materialize_to_temporary
  // CHECK: (accessor_decl {{.*}} get_for="computedVarGetterSetter" {{.*}} access=internal dynamic{{$}}
  // CHECK: (accessor_decl {{.*}} set_for="computedVarGetterSetter" {{.*}} access=internal dynamic{{$}}
  // CHECK: (accessor_decl {{.*}} _modify_for="computedVarGetterSetter" {{.*}} access=internal{{$}}
  dynamic var computedVarGetterSetter : Int {
    get {
      return 0
    }
    set {
    }
  }

  // CHECK: (var_decl {{.*}} "computedVarGetterModify" type='Int' interface type='Int' access=internal dynamic readImpl=getter writeImpl=modify_coroutine readWriteImpl=modify_coroutine
  // CHECK: (accessor_decl {{.*}} get_for="computedVarGetterModify" {{.*}} access=internal dynamic{{$}}
  // CHECK: (accessor_decl {{.*}} _modify_for="computedVarGetterModify" {{.*}} access=internal dynamic{{$}}
  // CHECK: (accessor_decl {{.*}} set_for="computedVarGetterModify" {{.*}} access=internal{{$}}
  dynamic var computedVarGetterModify : Int {
    get {
      return 0
    }
    _modify {
    }
  }

  // CHECK: (var_decl {{.*}} "computedVarReadSet" type='Int' interface type='Int' access=internal dynamic readImpl=read_coroutine writeImpl=setter readWriteImpl=materialize_to_temporary
  // CHECK: (accessor_decl {{.*}} _read_for="computedVarReadSet" {{.*}} access=internal dynamic{{$}}
  // CHECK: (accessor_decl {{.*}} set_for="computedVarReadSet" {{.*}} access=internal dynamic{{$}}
  // CHECK: (accessor_decl {{.*}} get_for="computedVarReadSet" {{.*}} access=internal{{$}}
  // CHECK: (accessor_decl {{.*}} _modify_for="computedVarReadSet" {{.*}} access=internal{{$}}
  dynamic var computedVarReadSet : Int {
    _read {
    }
    set {
    }
  }

  // CHECK: (var_decl {{.*}} "computedVarReadModify" type='Int' interface type='Int' access=internal dynamic readImpl=read_coroutine writeImpl=modify_coroutine readWriteImpl=modify_coroutine
  // CHECK: (accessor_decl {{.*}} _read_for="computedVarReadModify" {{.*}} access=internal dynamic{{$}}
  // CHECK: (accessor_decl {{.*}} _modify_for="computedVarReadModify" {{.*}} access=internal dynamic{{$}}
  // CHECK: (accessor_decl {{.*}} get_for="computedVarReadModify" {{.*}} access=internal{{$}}
  // CHECK: (accessor_decl {{.*}} set_for="computedVarReadModify" {{.*}} access=internal{{$}}
  dynamic var computedVarReadModify : Int {
    _read {
    }
    _modify {
    }
  }

  // CHECK: (var_decl {{.*}} "storedWithObserver" type='Int' interface type='Int' access=internal dynamic readImpl=stored writeImpl=stored_with_observers readWriteImpl=stored_with_didset
  // CHECK: (accessor_decl {{.*}} didSet_for="storedWithObserver" {{.*}} access=private dynamic{{$}}
  // CHECK: (accessor_decl {{.*}} get_for="storedWithObserver" {{.*}} access=internal dynamic{{$}}
  // CHECK: (accessor_decl {{.*}} set_for="storedWithObserver" {{.*}} access=internal{{$}}
  // CHECK: (accessor_decl {{.*}} _modify_for="storedWithObserver" {{.*}} access=internal{{$}}
  dynamic var storedWithObserver : Int {
    didSet {
    }
  }

  // CHECK: (func_decl {{.*}} access=internal dynamic
  dynamic func aMethod(arg: Int) -> Int {
    return arg
  }

  // CHECK: (subscript_decl {{.*}} "subscript(_:)" {{.*}} access=internal dynamic readImpl=getter writeImpl=setter readWriteImpl=materialize_to_temporary
  // CHECK: (accessor_decl {{.*}} get_for="subscript(_:)" {{.*}} access=internal dynamic{{$}}
  // CHECK: (accessor_decl {{.*}} set_for="subscript(_:)" {{.*}} access=internal dynamic{{$}}
  // CHECK: (accessor_decl {{.*}} _modify_for="subscript(_:)" {{.*}} access=internal{{$}}
  dynamic subscript(_ index: Int) -> Int {
    get {
      return 1
    }
    set {
    }
  }

  // CHECK: (subscript_decl {{.*}} "subscript(_:)" {{.*}} access=internal dynamic readImpl=getter writeImpl=modify_coroutine readWriteImpl=modify_coroutine
  // CHECK: (accessor_decl {{.*}} get_for="subscript(_:)" {{.*}} access=internal dynamic{{$}}
  // CHECK: (accessor_decl {{.*}} _modify_for="subscript(_:)" {{.*}} access=internal dynamic{{$}}
  // CHECK: (accessor_decl {{.*}} set_for="subscript(_:)" {{.*}} access=internal{{$}}
  dynamic subscript(_ index: Float) -> Int {
    get {
      return 1
    }
    _modify {
    }
  }

  // CHECK: (subscript_decl {{.*}} "subscript(_:)" {{.*}} access=internal dynamic readImpl=read_coroutine writeImpl=modify_coroutine readWriteImpl=modify_coroutine
  // CHECK: (accessor_decl {{.*}} _read_for="subscript(_:)" {{.*}} access=internal dynamic{{$}}
  // CHECK: (accessor_decl {{.*}} _modify_for="subscript(_:)" {{.*}} access=internal dynamic{{$}}
  // CHECK: (accessor_decl {{.*}} get_for="subscript(_:)" {{.*}} access=internal{{$}}
  // CHECK: (accessor_decl {{.*}} set_for="subscript(_:)" {{.*}} access=internal{{$}}
  dynamic subscript(_ index: Double) -> Int {
    _read {
    }
    _modify {
    }
  }

  // CHECK: (subscript_decl {{.*}} "subscript(_:)" {{.*}} access=internal dynamic readImpl=read_coroutine writeImpl=setter readWriteImpl=materialize_to_temporary
  // CHECK: (accessor_decl {{.*}} _read_for="subscript(_:)" {{.*}} access=internal dynamic{{$}}
  // CHECK: (accessor_decl {{.*}} set_for="subscript(_:)" {{.*}} access=internal dynamic{{$}}
  // CHECK: (accessor_decl {{.*}} get_for="subscript(_:)" {{.*}} access=internal{{$}}
  // CHECK: (accessor_decl {{.*}} _modify_for="subscript(_:)" {{.*}} access=internal{{$}}
  dynamic subscript(_ index: Strukt) -> Int {
    _read {
    }
    set {
    }
  }
}

class Klass {
  // CHECK: (class_decl {{.*}} "Klass"
  // CHECK: (var_decl {{.*}} "dynamicStorageOnlyVar" type='Int' interface type='Int' access=internal dynamic readImpl=stored writeImpl=stored readWriteImpl=stored
  // CHECK: (accessor_decl {{.*}} get_for="dynamicStorageOnlyVar" {{.*}} access=internal dynamic{{$}}
  // CHECK: (accessor_decl {{.*}} set_for="dynamicStorageOnlyVar" {{.*}} access=internal dynamic{{$}}
  // CHECK: (accessor_decl {{.*}} _modify_for="dynamicStorageOnlyVar" {{.*}} access=internal{{$}}
  dynamic var dynamicStorageOnlyVar : Int = 0

  // CHECK: (var_decl {{.*}} "computedVar" type='Int' interface type='Int' access=internal dynamic readImpl=getter immutable
  // CHECK: (accessor_decl {{.*}} get_for="computedVar" {{.*}} access=internal dynamic{{$}}
  dynamic var computedVar : Int {
    return 0
  }

  // CHECK: (var_decl {{.*}} "computedVar2" type='Int' interface type='Int' access=internal dynamic readImpl=getter immutable
  // CHECK: (accessor_decl {{.*}} get_for="computedVar2" {{.*}} access=internal dynamic{{$}}
  dynamic var computedVar2 : Int {
    get {
      return 0
    }
  }

  // CHECK: (var_decl {{.*}} "computedVarGetterSetter" type='Int' interface type='Int' access=internal dynamic readImpl=getter writeImpl=setter readWriteImpl=materialize_to_temporary
  // CHECK: (accessor_decl {{.*}} get_for="computedVarGetterSetter" {{.*}} access=internal dynamic{{$}}
  // CHECK: (accessor_decl {{.*}} set_for="computedVarGetterSetter" {{.*}} access=internal dynamic{{$}}
  // CHECK: (accessor_decl {{.*}} _modify_for="computedVarGetterSetter" {{.*}} access=internal{{$}}
  dynamic var computedVarGetterSetter : Int {
    get {
      return 0
    }
    set {
    }
  }

  // CHECK: (var_decl {{.*}} "computedVarGetterModify" type='Int' interface type='Int' access=internal dynamic readImpl=getter writeImpl=modify_coroutine readWriteImpl=modify_coroutine
  // CHECK: (accessor_decl {{.*}} get_for="computedVarGetterModify" {{.*}} access=internal dynamic{{$}}
  // CHECK: (accessor_decl {{.*}} _modify_for="computedVarGetterModify" {{.*}} access=internal dynamic{{$}}
  // CHECK: (accessor_decl {{.*}} set_for="computedVarGetterModify" {{.*}} access=internal{{$}}
  dynamic var computedVarGetterModify : Int {
    get {
      return 0
    }
    _modify {
    }
  }

  // CHECK: (var_decl {{.*}} "computedVarReadSet" type='Int' interface type='Int' access=internal dynamic readImpl=read_coroutine writeImpl=setter readWriteImpl=materialize_to_temporary
  // CHECK: (accessor_decl {{.*}} _read_for="computedVarReadSet" {{.*}} access=internal dynamic{{$}}
  // CHECK: (accessor_decl {{.*}} set_for="computedVarReadSet" {{.*}} access=internal dynamic{{$}}
  // CHECK: (accessor_decl {{.*}} get_for="computedVarReadSet" {{.*}} access=internal{{$}}
  // CHECK: (accessor_decl {{.*}} _modify_for="computedVarReadSet" {{.*}} access=internal{{$}}
  dynamic var computedVarReadSet : Int {
    _read {
    }
    set {
    }
  }

  // CHECK: (var_decl {{.*}} "computedVarReadModify" type='Int' interface type='Int' access=internal dynamic readImpl=read_coroutine writeImpl=modify_coroutine readWriteImpl=modify_coroutine
  // CHECK: (accessor_decl {{.*}} _read_for="computedVarReadModify" {{.*}} access=internal dynamic{{$}}
  // CHECK: (accessor_decl {{.*}} _modify_for="computedVarReadModify" {{.*}} access=internal dynamic{{$}}
  // CHECK: (accessor_decl {{.*}} get_for="computedVarReadModify" {{.*}} access=internal{{$}}
  // CHECK: (accessor_decl {{.*}} set_for="computedVarReadModify" {{.*}} access=internal{{$}}
  dynamic var computedVarReadModify : Int {
    _read {
    }
    _modify {
    }
  }
  // CHECK: (func_decl {{.*}} "aMethod(arg:)" {{.*}} access=internal dynamic
  dynamic func aMethod(arg: Int) -> Int {
    return arg
  }

  // CHECK-NOT: (func_decl {{.*}} "anotherMethod()" {{.*}} access=internal{{.*}} dynamic
  func anotherMethod() -> Int {
    return 3
  }

  // CHECK: (subscript_decl {{.*}} "subscript(_:)" {{.*}} access=internal dynamic readImpl=addressor writeImpl=mutable_addressor readWriteImpl=mutable_addressor
  // CHECK: (accessor_decl {{.*}} unsafeAddress_for="subscript(_:)" {{.*}} access=internal dynamic{{$}}
  // CHECK: (accessor_decl {{.*}} unsafeMutableAddress_for="subscript(_:)" {{.*}} access=internal dynamic{{$}}
  // CHECK: (accessor_decl {{.*}} get_for="subscript(_:)" {{.*}} access=internal{{$}}
  // CHECK: (accessor_decl {{.*}} set_for="subscript(_:)" {{.*}} access=internal{{$}}
  dynamic subscript(_ index: Int) -> Int {
    unsafeAddress {
      fatalError()
    }
    unsafeMutableAddress {
      fatalError()
    }
  }

  // CHECK: (subscript_decl {{.*}} "subscript(_:)" {{.*}} access=internal dynamic readImpl=getter writeImpl=mutable_addressor readWriteImpl=mutable_addressor
  // CHECK: (accessor_decl {{.*}} get_for="subscript(_:)" {{.*}} access=internal dynamic{{$}}
  // CHECK: (accessor_decl {{.*}} unsafeMutableAddress_for="subscript(_:)" {{.*}} access=internal dynamic{{$}}
  // CHECK: (accessor_decl {{.*}} set_for="subscript(_:)" {{.*}} access=internal{{$}}
  // CHECK: (accessor_decl {{.*}} _modify_for="subscript(_:)" {{.*}} access=internal{{$}}
  dynamic subscript(_ index: Float) -> Int {
    get {
      return 1
    }
    unsafeMutableAddress {
      fatalError()
    }
  }

  // CHECK: (subscript_decl {{.*}} "subscript(_:)" {{.*}} access=internal dynamic readImpl=read_coroutine writeImpl=mutable_addressor readWriteImpl=mutable_addressor
  // CHECK: (accessor_decl {{.*}} _read_for="subscript(_:)" {{.*}} access=internal dynamic{{$}}
  // CHECK: (accessor_decl {{.*}} unsafeMutableAddress_for="subscript(_:)" {{.*}} access=internal dynamic{{$}}
  // CHECK: (accessor_decl {{.*}} get_for="subscript(_:)" {{.*}} access=internal{{$}}
  // CHECK: (accessor_decl {{.*}} set_for="subscript(_:)" {{.*}} access=internal{{$}}
  // CHECK: (accessor_decl {{.*}} _modify_for="subscript(_:)" {{.*}} access=internal{{$}}
  dynamic subscript(_ index: Double) -> Int {
    _read {
    }
    unsafeMutableAddress {
      fatalError()
    }
  }

  // CHECK: (subscript_decl {{.*}} "subscript(_:)" {{.*}} access=internal dynamic readImpl=addressor writeImpl=setter readWriteImpl=materialize_to_temporary
  // CHECK: (accessor_decl {{.*}} unsafeAddress_for="subscript(_:)" {{.*}} access=internal dynamic{{$}}
  // CHECK: (accessor_decl {{.*}} set_for="subscript(_:)" {{.*}} access=internal dynamic{{$}}
  // CHECK: (accessor_decl {{.*}} get_for="subscript(_:)" {{.*}} access=internal{{$}}
  // CHECK: (accessor_decl {{.*}} _modify_for="subscript(_:)" {{.*}} access=internal{{$}}
  dynamic subscript(_ index: Int8) -> Int {
    unsafeAddress {
      fatalError()
    }
    set {
    }
  }

  // CHECK: (subscript_decl {{.*}} "subscript(_:)" {{.*}} access=internal dynamic readImpl=addressor writeImpl=modify_coroutine readWriteImpl=modify_coroutine
  // CHECK: (accessor_decl {{.*}} unsafeAddress_for="subscript(_:)" {{.*}} access=internal dynamic{{$}}
  // CHECK: (accessor_decl {{.*}} _modify_for="subscript(_:)" {{.*}} access=internal dynamic{{$}}
  // CHECK: (accessor_decl {{.*}} get_for="subscript(_:)" {{.*}} access=internal{{$}}
  // CHECK: (accessor_decl {{.*}} set_for="subscript(_:)" {{.*}} access=internal{{$}}
  dynamic subscript(_ index: Int16) -> Int {
    unsafeAddress {
      fatalError()
    }
    _modify {
    }
  }
}

class SubKlass : Klass {

  // CHECK: (class_decl {{.*}} "SubKlass"
  // CHECK: (func_decl {{.*}} "aMethod(arg:)" interface type='(SubKlass) -> (Int) -> Int' access=internal {{.*}} dynamic
  override dynamic func aMethod(arg: Int) -> Int {
   return 23
  }

  // CHECK: (func_decl {{.*}} "anotherMethod()" interface type='(SubKlass) -> () -> Int' access=internal {{.*}} dynamic
  override dynamic func anotherMethod() -> Int {
   return 23
  }
}
