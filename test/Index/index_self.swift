// RUN: %target-swift-ide-test -print-indexed-symbols -source-filename %s | %FileCheck %s

struct Foo { // CHECK: [[@LINE]]:8 | struct/Swift | Foo | [[Foo_USR:.*]] | Def | rel: 0
  init() {} // CHECK: [[@LINE]]:3 | constructor/Swift | init() | [[Foo_init_USR:.*]] | Def,RelChild | rel: 1

  static func bar() -> Self {
    .init() // CHECK: [[@LINE]]:6 | constructor/Swift | init() | [[Foo_init_USR]] | Ref,Call,RelCall,RelCont | rel: 1
  }

  static func baz() -> Self {
    Self()
    // CHECK: [[@LINE-1]]:5 | struct/Swift | Foo | [[Foo_USR]] | Ref,Impl,RelCont | rel: 1
    // CHECK: [[@LINE-2]]:5 | constructor/Swift | init() | [[Foo_init_USR]] | Ref,Call,RelCall,RelCont | rel: 1
  }
}

final class Final { // CHECK: [[@LINE]]:13 | class/Swift | Final | [[Final_USR:.*]] | Def | rel: 0
  init() {} // CHECK: [[@LINE]]:3 | constructor/Swift | init() | [[Final_init_USR:.*]] | Def,RelChild | rel: 1

  static func foo() -> Self {
    .init() // CHECK: [[@LINE]]:6 | constructor/Swift | init() | [[Final_init_USR]] | Ref,Call,RelCall,RelCont | rel: 1
  }

  static func baz() -> Self {
    Self()
    // CHECK: [[@LINE-1]]:5 | class/Swift | Final | [[Final_USR]] | Ref,Impl,RelCont | rel: 1
    // CHECK: [[@LINE-2]]:5 | constructor/Swift | init() | [[Final_init_USR]] | Ref,Call,RelCall,RelCont | rel: 1
  }
}

class Bar { // CHECK: [[@LINE]]:7 | class/Swift | Bar | [[Bar_USR:.*]] | Def | rel: 0
  required init() {} // CHECK: [[@LINE]]:12 | constructor/Swift | init() | [[Bar_init_USR:.*]] | Def,RelChild | rel: 1

  static func foo() -> Self {
    .init() // CHECK: [[@LINE]]:6 | constructor/Swift | init() | [[Bar_init_USR]] | Ref,Call,RelCall,RelCont | rel: 1
  }

  static func baz() -> Self {
    Self()
    // CHECK: [[@LINE-1]]:5 | class/Swift | Bar | [[Bar_USR]] | Ref,Impl,RelCont | rel: 1
    // TODO: This reference should be dynamic
    // CHECK: [[@LINE-3]]:5 | constructor/Swift | init() | [[Bar_init_USR]] | Ref,Call,RelCall,RelCont | rel: 1
  }
}

class Baz: Bar {}

protocol Proto { // CHECK: [[@LINE]]:10 | protocol/Swift | Proto | [[Proto_USR:.*]] | Def | rel: 0
  init() // CHECK: [[@LINE]]:3 | constructor/Swift | init() | [[Proto_init_USR:.*]] | Def,RelChild | rel: 1
}

extension Proto {
  func foo() -> Self {
    // TODO: This reference should be dynamic
    Self() // CHECK: [[@LINE]]:5 | constructor/Swift | init() | [[Proto_init_USR]] | Ref,Call,RelCall,RelCont | rel: 1
  }
}
