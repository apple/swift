// RUN: %target-swift-frontend -swift-version 5 -typecheck -dump-ast %s | %FileCheck  %s

struct Strukt {
  // CHECK: (struct_decl {{.*}} "Strukt"
  // CHECK: (var_decl {{.*}} "dynamicStorageOnlyVar" interface type='Int' access=internal dynamic readImpl=stored writeImpl=stored readWriteImpl=stored
  // CHECK: (accessor_decl {{.*}} access=internal dynamic get_for=dynamicStorageOnlyVar
  // CHECK: (accessor_decl {{.*}} access=internal dynamic set_for=dynamicStorageOnlyVar
  // CHECK: (accessor_decl {{.*}} access=internal _modify_for=dynamicStorageOnlyVar
  dynamic var dynamicStorageOnlyVar : Int = 0

  // CHECK: (var_decl {{.*}} "computedVar" interface type='Int' access=internal dynamic readImpl=getter immutable
  // CHECK: (accessor_decl {{.*}} access=internal dynamic get_for=computedVar
  dynamic var computedVar : Int {
    return 0
  }

  // CHECK: (var_decl {{.*}} "computedVar2" interface type='Int' access=internal dynamic readImpl=getter immutable
  // CHECK: (accessor_decl {{.*}} access=internal dynamic get_for=computedVar2
  dynamic var computedVar2 : Int {
    get {
      return 0
    }
  }

  // CHECK: (var_decl {{.*}} "computedVarGetterSetter" interface type='Int' access=internal dynamic readImpl=getter writeImpl=setter readWriteImpl=materialize_to_temporary
  // CHECK: (accessor_decl {{.*}} access=internal dynamic get_for=computedVarGetterSetter
  // CHECK: (accessor_decl {{.*}} access=internal dynamic set_for=computedVarGetterSetter
  // CHECK: (accessor_decl {{.*}} access=internal _modify_for=computedVarGetterSetter
  dynamic var computedVarGetterSetter : Int {
    get {
      return 0
    }
    set {
    }
  }

  // CHECK: (var_decl {{.*}} "computedVarGetterModify" interface type='Int' access=internal dynamic readImpl=getter writeImpl=modify_coroutine readWriteImpl=modify_coroutine
  // CHECK: (accessor_decl {{.*}} access=internal dynamic get_for=computedVarGetterModify
  // CHECK: (accessor_decl {{.*}} access=internal dynamic _modify_for=computedVarGetterModify
  // CHECK: (accessor_decl {{.*}} access=internal set_for=computedVarGetterModify
  dynamic var computedVarGetterModify : Int {
    get {
      return 0
    }
    _modify {
    }
  }

  // CHECK: (var_decl {{.*}} "computedVarReadSet" interface type='Int' access=internal dynamic readImpl=read_coroutine writeImpl=setter readWriteImpl=materialize_to_temporary
  // CHECK: (accessor_decl {{.*}} access=internal dynamic _read_for=computedVarReadSet
  // CHECK: (accessor_decl {{.*}} access=internal dynamic set_for=computedVarReadSet
  // CHECK: (accessor_decl {{.*}} access=internal get_for=computedVarReadSet
  // CHECK: (accessor_decl {{.*}} access=internal _modify_for=computedVarReadSet
  dynamic var computedVarReadSet : Int {
    _read {
    }
    set {
    }
  }

  // CHECK: (var_decl {{.*}} "computedVarReadModify" interface type='Int' access=internal dynamic readImpl=read_coroutine writeImpl=modify_coroutine readWriteImpl=modify_coroutine
  // CHECK: (accessor_decl {{.*}} access=internal dynamic _read_for=computedVarReadModify
  // CHECK: (accessor_decl {{.*}} access=internal dynamic _modify_for=computedVarReadModify
  // CHECK: (accessor_decl {{.*}} access=internal get_for=computedVarReadModify
  // CHECK: (accessor_decl {{.*}} access=internal set_for=computedVarReadModify
  dynamic var computedVarReadModify : Int {
    _read {
    }
    _modify {
    }
  }

  // CHECK: (var_decl {{.*}} "storedWithObserver" interface type='Int' access=internal dynamic readImpl=stored writeImpl=stored_with_observers readWriteImpl=stored_with_didset
  // CHECK: (accessor_decl {{.*}}access=private dynamic didSet_for=storedWithObserver
  // CHECK: (accessor_decl {{.*}}access=internal dynamic get_for=storedWithObserver
  // CHECK: (accessor_decl {{.*}}access=internal set_for=storedWithObserver
  // CHECK: (accessor_decl {{.*}}access=internal _modify_for=storedWithObserver
  dynamic var storedWithObserver : Int {
    didSet {
    }
  }

  // CHECK: (func_decl {{.*}} access=internal dynamic
  dynamic func aMethod(arg: Int) -> Int {
    return arg
  }

  // CHECK: (subscript_decl {{.*}} "subscript(_:)" {{.*}} access=internal dynamic readImpl=getter writeImpl=setter readWriteImpl=materialize_to_temporary
  // CHECK: (accessor_decl {{.*}} access=internal dynamic get_for=subscript(_:)
  // CHECK: (accessor_decl {{.*}} access=internal dynamic set_for=subscript(_:)
  // CHECK: (accessor_decl {{.*}} access=internal _modify_for=subscript(_:)
  dynamic subscript(_ index: Int) -> Int {
    get {
      return 1
    }
    set {
    }
  }

  // CHECK: (subscript_decl {{.*}} "subscript(_:)" {{.*}} access=internal dynamic readImpl=getter writeImpl=modify_coroutine readWriteImpl=modify_coroutine
  // CHECK: (accessor_decl {{.*}} access=internal dynamic get_for=subscript(_:)
  // CHECK: (accessor_decl {{.*}} access=internal dynamic _modify_for=subscript(_:)
  // CHECK: (accessor_decl {{.*}} access=internal set_for=subscript(_:)
  dynamic subscript(_ index: Float) -> Int {
    get {
      return 1
    }
    _modify {
    }
  }

  // CHECK: (subscript_decl {{.*}} "subscript(_:)" {{.*}} access=internal dynamic readImpl=read_coroutine writeImpl=modify_coroutine readWriteImpl=modify_coroutine
  // CHECK: (accessor_decl {{.*}} access=internal dynamic _read_for=subscript(_:)
  // CHECK: (accessor_decl {{.*}} access=internal dynamic _modify_for=subscript(_:)
  // CHECK: (accessor_decl {{.*}} access=internal get_for=subscript(_:)
  // CHECK: (accessor_decl {{.*}} access=internal set_for=subscript(_:)
  dynamic subscript(_ index: Double) -> Int {
    _read {
    }
    _modify {
    }
  }

  // CHECK: (subscript_decl {{.*}} "subscript(_:)" {{.*}} access=internal dynamic readImpl=read_coroutine writeImpl=setter readWriteImpl=materialize_to_temporary
  // CHECK: (accessor_decl {{.*}} access=internal dynamic _read_for=subscript(_:)
  // CHECK: (accessor_decl {{.*}} access=internal dynamic set_for=subscript(_:)
  // CHECK: (accessor_decl {{.*}} access=internal get_for=subscript(_:)
  // CHECK: (accessor_decl {{.*}} access=internal _modify_for=subscript(_:)
  dynamic subscript(_ index: Strukt) -> Int {
    _read {
    }
    set {
    }
  }
}

class Klass {
  // CHECK: (class_decl {{.*}} "Klass"
  // CHECK: (var_decl {{.*}} "dynamicStorageOnlyVar" interface type='Int' access=internal dynamic readImpl=stored writeImpl=stored readWriteImpl=stored
  // CHECK: (accessor_decl {{.*}} access=internal dynamic get_for=dynamicStorageOnlyVar
  // CHECK: (accessor_decl {{.*}} access=internal dynamic set_for=dynamicStorageOnlyVar
  // CHECK: (accessor_decl {{.*}} access=internal _modify_for=dynamicStorageOnlyVar
  dynamic var dynamicStorageOnlyVar : Int = 0

  // CHECK: (var_decl {{.*}} "computedVar" interface type='Int' access=internal dynamic readImpl=getter immutable
  // CHECK: (accessor_decl {{.*}} access=internal dynamic get_for=computedVar
  dynamic var computedVar : Int {
    return 0
  }

  // CHECK: (var_decl {{.*}} "computedVar2" interface type='Int' access=internal dynamic readImpl=getter immutable
  // CHECK: (accessor_decl {{.*}} access=internal dynamic get_for=computedVar2
  dynamic var computedVar2 : Int {
    get {
      return 0
    }
  }

  // CHECK: (var_decl {{.*}} "computedVarGetterSetter" interface type='Int' access=internal dynamic readImpl=getter writeImpl=setter readWriteImpl=materialize_to_temporary
  // CHECK: (accessor_decl {{.*}} access=internal dynamic get_for=computedVarGetterSetter
  // CHECK: (accessor_decl {{.*}} access=internal dynamic set_for=computedVarGetterSetter
  // CHECK: (accessor_decl {{.*}} access=internal _modify_for=computedVarGetterSetter
  dynamic var computedVarGetterSetter : Int {
    get {
      return 0
    }
    set {
    }
  }

  // CHECK: (var_decl {{.*}} "computedVarGetterModify" interface type='Int' access=internal dynamic readImpl=getter writeImpl=modify_coroutine readWriteImpl=modify_coroutine
  // CHECK: (accessor_decl {{.*}} access=internal dynamic get_for=computedVarGetterModify
  // CHECK: (accessor_decl {{.*}} access=internal dynamic _modify_for=computedVarGetterModify
  // CHECK: (accessor_decl {{.*}} access=internal set_for=computedVarGetterModify
  dynamic var computedVarGetterModify : Int {
    get {
      return 0
    }
    _modify {
    }
  }

  // CHECK: (var_decl {{.*}} "computedVarReadSet" interface type='Int' access=internal dynamic readImpl=read_coroutine writeImpl=setter readWriteImpl=materialize_to_temporary
  // CHECK: (accessor_decl {{.*}} access=internal dynamic _read_for=computedVarReadSet
  // CHECK: (accessor_decl {{.*}} access=internal dynamic set_for=computedVarReadSet
  // CHECK: (accessor_decl {{.*}} access=internal get_for=computedVarReadSet
  // CHECK: (accessor_decl {{.*}} access=internal _modify_for=computedVarReadSet
  dynamic var computedVarReadSet : Int {
    _read {
    }
    set {
    }
  }

  // CHECK: (var_decl {{.*}} "computedVarReadModify" interface type='Int' access=internal dynamic readImpl=read_coroutine writeImpl=modify_coroutine readWriteImpl=modify_coroutine
  // CHECK: (accessor_decl {{.*}} access=internal dynamic _read_for=computedVarReadModify
  // CHECK: (accessor_decl {{.*}} access=internal dynamic _modify_for=computedVarReadModify
  // CHECK: (accessor_decl {{.*}} access=internal get_for=computedVarReadModify
  // CHECK: (accessor_decl {{.*}} access=internal set_for=computedVarReadModify
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
  // CHECK: (accessor_decl {{.*}} access=internal dynamic unsafeAddress_for=subscript(_:)
  // CHECK: (accessor_decl {{.*}} access=internal dynamic unsafeMutableAddress_for=subscript(_:)
  // CHECK: (accessor_decl {{.*}} access=internal get_for=subscript(_:)
  // CHECK: (accessor_decl {{.*}} access=internal set_for=subscript(_:)
  dynamic subscript(_ index: Int) -> Int {
    unsafeAddress {
      fatalError()
    }
    unsafeMutableAddress {
      fatalError()
    }
  }

  // CHECK: (subscript_decl {{.*}} "subscript(_:)" {{.*}} access=internal dynamic readImpl=getter writeImpl=mutable_addressor readWriteImpl=mutable_addressor
  // CHECK: (accessor_decl {{.*}} access=internal dynamic get_for=subscript(_:)
  // CHECK: (accessor_decl {{.*}} access=internal dynamic unsafeMutableAddress_for=subscript(_:)
  // CHECK: (accessor_decl {{.*}} access=internal set_for=subscript(_:)
  // CHECK: (accessor_decl {{.*}} access=internal _modify_for=subscript(_:)
  dynamic subscript(_ index: Float) -> Int {
    get {
      return 1
    }
    unsafeMutableAddress {
      fatalError()
    }
  }

  // CHECK: (subscript_decl {{.*}} "subscript(_:)" {{.*}} access=internal dynamic readImpl=read_coroutine writeImpl=mutable_addressor readWriteImpl=mutable_addressor
  // CHECK: (accessor_decl {{.*}} access=internal dynamic _read_for=subscript(_:)
  // CHECK: (accessor_decl {{.*}} access=internal dynamic unsafeMutableAddress_for=subscript(_:)
  // CHECK: (accessor_decl {{.*}} access=internal get_for=subscript(_:)
  // CHECK: (accessor_decl {{.*}} access=internal set_for=subscript(_:)
  // CHECK: (accessor_decl {{.*}} access=internal _modify_for=subscript(_:)
  dynamic subscript(_ index: Double) -> Int {
    _read {
    }
    unsafeMutableAddress {
      fatalError()
    }
  }

  // CHECK: (subscript_decl {{.*}} "subscript(_:)" {{.*}} access=internal dynamic readImpl=addressor writeImpl=setter readWriteImpl=materialize_to_temporary
  // CHECK: (accessor_decl {{.*}} access=internal dynamic unsafeAddress_for=subscript(_:)
  // CHECK: (accessor_decl {{.*}} access=internal dynamic set_for=subscript(_:)
  // CHECK: (accessor_decl {{.*}} access=internal get_for=subscript(_:)
  // CHECK: (accessor_decl {{.*}} access=internal _modify_for=subscript(_:)
  dynamic subscript(_ index: Int8) -> Int {
    unsafeAddress {
      fatalError()
    }
    set {
    }
  }

  // CHECK: (subscript_decl {{.*}} "subscript(_:)" {{.*}} access=internal dynamic readImpl=addressor writeImpl=modify_coroutine readWriteImpl=modify_coroutine
  // CHECK: (accessor_decl {{.*}} access=internal dynamic unsafeAddress_for=subscript(_:)
  // CHECK: (accessor_decl {{.*}} access=internal dynamic _modify_for=subscript(_:)
  // CHECK: (accessor_decl {{.*}} access=internal get_for=subscript(_:)
  // CHECK: (accessor_decl {{.*}} access=internal set_for=subscript(_:)
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
