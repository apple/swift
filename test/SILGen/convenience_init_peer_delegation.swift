// RUN: %target-swift-emit-silgen -enable-objc-interop -disable-objc-attr-requires-foundation-module %s | %FileCheck %s

class X {
  init() {
  }

  // Convenience inits must dynamically dispatch designated inits...
  // CHECK-LABEL: sil hidden [ossa] @$s32convenience_init_peer_delegation1XC0A0ACyt_tcfC
  // CHECK:         class_methddod {{%.*}}, #X.init!allocator
  convenience init(convenience: ()) {
    self.init()
  }

  // ...but can statically invoke peer convenience inits
  // CHECK-LABEL: sil hidden [ossa] @$s32convenience_init_peer_delegation1XC17doubleConvenienceACyt_tcfC
  // CHECK:         function_ref @$s32convenience_init_peer_delegation1XC0A0ACyt_tcfC
  convenience init(doubleConvenience: ()) {
    self.init(convenience: ())
  }

  // CHECK-LABEL: sil hidden [exact_self_class] [ossa] @$s32convenience_init_peer_delegation1XC8requiredACyt_tcfC
  required init(required: ()) {
  }

  // CHECK-LABEL: sil hidden [ossa] @$s32convenience_init_peer_delegation1XC19requiredConvenienceACyt_tcfC
  required convenience init(requiredConvenience: ()) {
    self.init(required: ())
  }

  // Convenience inits must dynamically dispatch required peer convenience inits
  // CHECK-LABEL: sil hidden [ossa] @$s32convenience_init_peer_delegation1XC25requiredDoubleConvenienceACyt_tcfC
  // CHECK:         class_method {{%.*}}, #X.init!allocator
  required convenience init(requiredDoubleConvenience: ()) {
    self.init(requiredDoubleConvenience: ())
  }
}

class Y: X {
  // This is really a designated initializer. Ensure that we don't try to
  // treat it as an override of the base class convenience initializer (and
  // override a nonexistent vtable entry) just because it has the same name.
  init(convenience: ()) {
    super.init()
  }

  // Conversely, a designated init *can* be overridden as a convenience
  // initializer.
  override convenience init() {
    self.init(convenience: ())
  }

  required init(required: ()) { super.init() }
  required init(requiredConvenience: ()) { super.init() }
  required init(requiredDoubleConvenience: ()) { super.init() }
}

// CHECK-LABEL: sil hidden [ossa] @$s32convenience_init_peer_delegation11invocations2xtyAA1XCm_tF
func invocations(xt: X.Type) {
  // CHECK: function_ref @$s32convenience_init_peer_delegation1XCACycfC
  _ = X()
  // CHECK: function_ref @$s32convenience_init_peer_delegation1XC0A0ACyt_tcfC
  _ = X(convenience: ())
  // CHECK: function_ref @$s32convenience_init_peer_delegation1XC17doubleConvenienceACyt_tcfC
  _ = X(doubleConvenience: ())
  // CHECK: function_ref @$s32convenience_init_peer_delegation1XC8requiredACyt_tcfC
  _ = X(required: ())
  // CHECK: function_ref @$s32convenience_init_peer_delegation1XC19requiredConvenienceACyt_tcfC
  _ = X(requiredConvenience: ())
  // CHECK: function_ref @$s32convenience_init_peer_delegation1XC25requiredDoubleConvenienceACyt_tcfC
  _ = X(requiredDoubleConvenience: ())

  // CHECK: class_method {{%.*}}, #X.init!allocator
  _ = xt.init(required: ())
  // CHECK: class_method {{%.*}}, #X.init!allocator
  _ = xt.init(requiredConvenience: ())
  // CHECK: class_method {{%.*}}, #X.init!allocator
  _ = xt.init(requiredDoubleConvenience: ())
}

class ObjCBase {
  init(swift: ()) {}
  @objc(initAsObjC) init(objc: ()) {}

  // CHECK-LABEL: sil hidden [ossa] @$s32convenience_init_peer_delegation8ObjCBaseC11objcToSwiftACyt_tcfC
  // CHECK: function_ref @$s32convenience_init_peer_delegation8ObjCBaseC11objcToSwiftACyt_tcfc :
  // CHECK: end sil function '$s32convenience_init_peer_delegation8ObjCBaseC11objcToSwiftACyt_tcfC'
  // CHECK-LABEL: sil hidden [ossa] @$s32convenience_init_peer_delegation8ObjCBaseC11objcToSwiftACyt_tcfc
  // CHECK: class_method {{%.+}} : $@thick ObjCBase.Type, #ObjCBase.init!allocator
  // CHECK: end sil function '$s32convenience_init_peer_delegation8ObjCBaseC11objcToSwiftACyt_tcfc'
  // CHECK-LABEL: sil hidden [thunk] [ossa] @$s32convenience_init_peer_delegation8ObjCBaseC11objcToSwiftACyt_tcfcTo
  // CHECK: function_ref @$s32convenience_init_peer_delegation8ObjCBaseC11objcToSwiftACyt_tcfc :
  // CHECK: end sil function '$s32convenience_init_peer_delegation8ObjCBaseC11objcToSwiftACyt_tcfcTo'
  @objc convenience init(objcToSwift: ()) {
    self.init(swift: ())
  }

  // CHECK-LABEL: sil hidden [ossa] @$s32convenience_init_peer_delegation8ObjCBaseC07swiftToE1CACyt_tcfC
  // CHECK: class_method %0 : $@thick ObjCBase.Type, #ObjCBase.init!allocator
  // CHECK: end sil function '$s32convenience_init_peer_delegation8ObjCBaseC07swiftToE1CACyt_tcfC'
  convenience init(swiftToObjC: ()) {
    self.init(objc: ())
  }

  // CHECK-LABEL: sil hidden [ossa] @$s32convenience_init_peer_delegation8ObjCBaseC06objcToE1CACyt_tcfC
  // CHECK: function_ref @$s32convenience_init_peer_delegation8ObjCBaseC06objcToE1CACyt_tcfc :
  // CHECK: end sil function '$s32convenience_init_peer_delegation8ObjCBaseC06objcToE1CACyt_tcfC'
  // CHECK-LABEL: sil hidden [ossa] @$s32convenience_init_peer_delegation8ObjCBaseC06objcToE1CACyt_tcfc
  // CHECK: objc_method {{%.+}} : $ObjCBase, #ObjCBase.init!initializer.foreign
  // CHECK: end sil function '$s32convenience_init_peer_delegation8ObjCBaseC06objcToE1CACyt_tcfc'
  // CHECK-LABEL: sil hidden [thunk] [ossa] @$s32convenience_init_peer_delegation8ObjCBaseC06objcToE1CACyt_tcfcTo
  // CHECK: function_ref @$s32convenience_init_peer_delegation8ObjCBaseC06objcToE1CACyt_tcfc :
  // CHECK: end sil function '$s32convenience_init_peer_delegation8ObjCBaseC06objcToE1CACyt_tcfcTo'
  @objc convenience init(objcToObjC: ()) {
    self.init(objc: ())
  }

  // CHECK-LABEL: sil hidden [ossa] @$s32convenience_init_peer_delegation8ObjCBaseC22objcToSwiftConvenienceACyt_tcfC
  // CHECK: function_ref @$s32convenience_init_peer_delegation8ObjCBaseC22objcToSwiftConvenienceACyt_tcfc :
  // CHECK: end sil function '$s32convenience_init_peer_delegation8ObjCBaseC22objcToSwiftConvenienceACyt_tcfC'
  // CHECK-LABEL: sil hidden [ossa] @$s32convenience_init_peer_delegation8ObjCBaseC22objcToSwiftConvenienceACyt_tcfc
  // CHECK: function_ref @$s32convenience_init_peer_delegation8ObjCBaseC07swiftToE1CACyt_tcfC :
  // CHECK: end sil function '$s32convenience_init_peer_delegation8ObjCBaseC22objcToSwiftConvenienceACyt_tcfc'
  // CHECK-LABEL: sil hidden [thunk] [ossa] @$s32convenience_init_peer_delegation8ObjCBaseC22objcToSwiftConvenienceACyt_tcfcTo
  // CHECK: function_ref @$s32convenience_init_peer_delegation8ObjCBaseC22objcToSwiftConvenienceACyt_tcfc :
  // CHECK: end sil function '$s32convenience_init_peer_delegation8ObjCBaseC22objcToSwiftConvenienceACyt_tcfcTo'
  @objc convenience init(objcToSwiftConvenience: ()) {
    self.init(swiftToObjC: ())
  }

  // CHECK-LABEL: sil hidden [ossa] @$s32convenience_init_peer_delegation8ObjCBaseC07swiftToE12CConvenienceACyt_tcfC
  // CHECK: function_ref @$s32convenience_init_peer_delegation8ObjCBaseC11objcToSwiftACyt_tcfC
  // CHECK: end sil function '$s32convenience_init_peer_delegation8ObjCBaseC07swiftToE12CConvenienceACyt_tcfC'
  convenience init(swiftToObjCConvenience: ()) {
    self.init(objcToSwift: ())
  }

  // CHECK-LABEL: sil hidden [ossa] @$s32convenience_init_peer_delegation8ObjCBaseC06objcToE12CConvenienceACyt_tcfC
  // CHECK: function_ref @$s32convenience_init_peer_delegation8ObjCBaseC06objcToE12CConvenienceACyt_tcfc :
  // CHECK: end sil function '$s32convenience_init_peer_delegation8ObjCBaseC06objcToE12CConvenienceACyt_tcfC'
  // CHECK-LABEL: sil hidden [ossa] @$s32convenience_init_peer_delegation8ObjCBaseC06objcToE12CConvenienceACyt_tcfc
  // CHECK: objc_method {{%.+}} : $ObjCBase, #ObjCBase.init!initializer.foreign
  // CHECK: end sil function '$s32convenience_init_peer_delegation8ObjCBaseC06objcToE12CConvenienceACyt_tcfc'
  // CHECK-LABEL: sil hidden [thunk] [ossa] @$s32convenience_init_peer_delegation8ObjCBaseC06objcToE12CConvenienceACyt_tcfcTo
  // CHECK: function_ref @$s32convenience_init_peer_delegation8ObjCBaseC06objcToE12CConvenienceACyt_tcfc :
  // CHECK: end sil function '$s32convenience_init_peer_delegation8ObjCBaseC06objcToE12CConvenienceACyt_tcfcTo'
  @objc convenience init(objcToObjCConvenience: ()) {
    self.init(objcToObjC: ())
  }
}

// CHECK-LABEL: sil_vtable X
//                -- designated init()
// CHECK:         @$s32convenience_init_peer_delegation1XCACycfC
// CHECK-NOT:     @$s32convenience_init_peer_delegation1XCACycfc

//                -- no unrequired convenience inits
// CHECK-NOT:     @$s32convenience_init_peer_delegation1XC0A0ACyt_tcfC
// CHECK-NOT:     @$s32convenience_init_peer_delegation1XC0A0ACyt_tcfc
// CHECK-NOT:     @$s32convenience_init_peer_delegation1XC17doubleConvenienceACyt_tcfC
// CHECK-NOT:     @$s32convenience_init_peer_delegation1XC17doubleConvenienceACyt_tcfc

//                -- designated init(required:)
// CHECK:         @$s32convenience_init_peer_delegation1XC8requiredACyt_tcfC
// CHECK-NOT:     @$s32convenience_init_peer_delegation1XC8requiredACyt_tcfc
//                -- convenience init(requiredConvenience:)
// CHECK:         @$s32convenience_init_peer_delegation1XC19requiredConvenienceACyt_tcfC
// CHECK-NOT:     @$s32convenience_init_peer_delegation1XC19requiredConvenienceACyt_tcfc
//                -- convenience init(requiredDoubleConvenience:)
// CHECK:         @$s32convenience_init_peer_delegation1XC25requiredDoubleConvenienceACyt_tcfC
// CHECK-NOT:     @$s32convenience_init_peer_delegation1XC25requiredDoubleConvenienceACyt_tcfc

// CHECK-LABEL: sil_vtable Y
//                -- designated init() overridden by convenience init
// CHECK:         @$s32convenience_init_peer_delegation1YCACycfC
// CHECK-NOT:     @$s32convenience_init_peer_delegation1YCACycfc
//                -- Y.init(convenience:) is a designated init
// CHECK:         @$s32convenience_init_peer_delegation1YC0A0ACyt_tcfC
// CHECK-NOT:     @$s32convenience_init_peer_delegation1YC0A0ACyt_tcfc
