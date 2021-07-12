// RUN: %target-swift-frontend -typecheck -dump-ast %s | %FileCheck %s

@propertyWrapper
struct Wrapper<T> {
  var wrappedValue: T
}

protocol DefaultInit {
  init()
}

// CHECK: struct_decl{{.*}}"UseWrapper"
struct UseWrapper<T: DefaultInit> {
  // CHECK: var_decl{{.*}}"wrapped"

  // CHECK: accessor_decl{{.*}}get_for="wrapped"
  // CHECK: member_ref_expr{{.*}}UseWrapper._wrapped

  // CHECK: accessor_decl{{.*}}set_for="wrapped"
  // CHECK: member_ref_expr{{.*}}UseWrapper._wrapped

  // CHECK: accessor_decl{{.*}}_modify_for="wrapped"
  // CHECK: yield_stmt
  // CHECK: member_ref_expr{{.*}}Wrapper.wrappedValue
  @Wrapper
  var wrapped = T()

  // CHECK: pattern_binding_decl implicit
  // CHECK-NEXT: (pattern_entry
  // CHECK-NEXT: (pattern=pattern_typed implicit type='Wrapper<T>'
  // CHECK-NEXT: (pattern_named implicit type='Wrapper<T>' "_wrapped"
  // CHECK: constructor_ref_call_expr
  // CHECK-NEXT: declref_expr{{.*}}Wrapper.init(wrappedValue:)
  init() { }
}

struct UseWillSetDidSet {
  // CHECK: var_decl{{.*}}"z"

  // CHECK: accessor_decl{{.*}}set_for="z"
  // CHECK: member_ref_expr{{.*}}UseWillSetDidSet._z
  @Wrapper
  var z: Int {
    willSet {
      print(newValue)
    }

    didSet {
      print(oldValue)
    }
  }
}

@propertyWrapper
struct Observable<Value> {
  private var stored: Value

  init(initialValue: Value) {
    self.stored = initialValue
  }

  var wrappedValue: Value {
    get { fatalError("called wrappedValue getter") }
    set { fatalError("called wrappedValue setter") }
  }
  
  static subscript<EnclosingSelf>(
      _enclosingInstance observed: EnclosingSelf,
      wrapped wrappedKeyPath: ReferenceWritableKeyPath<EnclosingSelf, Value>,
      storage storageKeyPath: ReferenceWritableKeyPath<EnclosingSelf, Self>
    ) -> Value {
    get {
      observed[keyPath: storageKeyPath].stored
    }
    set {
      observed[keyPath: storageKeyPath].stored = newValue
    }
  }
}

// CHECK-LABEL: class_decl{{.*}}"MyObservedType"
class MyObservedType {
  @Observable var observedProperty = 17

  // CHECK: accessor_decl{{.*}}get_for="observedProperty"
  // CHECK:   subscript_expr implicit type='@lvalue Int' decl={{.*}}.Observable.subscript(_enclosingInstance:wrapped:storage:)

  // CHECK: accessor_decl{{.*}}set_for="observedProperty"
  // CHECK:   subscript_expr implicit type='@lvalue Int' decl={{.*}}.Observable.subscript(_enclosingInstance:wrapped:storage:)
}

