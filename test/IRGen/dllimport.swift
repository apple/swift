// RUN: %swift -Xllvm -sil-disable-pass=GenericSpecializer -target thumbv7--windows-itanium -emit-ir -parse-as-library -disable-legacy-type-info -parse-stdlib -module-name dllimport %s -o - -enable-source-import -I %S | %FileCheck %s -check-prefix CHECK -check-prefix CHECK-NO-OPT
// RUN: %swift -Xllvm -sil-disable-pass=GenericSpecializer -target thumbv7--windows-itanium -O -emit-ir -parse-as-library -disable-legacy-type-info -parse-stdlib -module-name dllimport -primary-file %s -o - -enable-source-import -I %S | %FileCheck %s -check-prefix CHECK -check-prefix CHECK-OPT

// REQUIRES: CODEGENERATOR=ARM

import dllexport

public func get_ci() -> dllexport.c {
  return dllexport.ci
}

public func get_c_type() -> dllexport.c.Type {
  return dllexport.c.self
}

public class d : c {
  override init() {
    super.init()
  }

  @inline(never)
  func f(_ : dllexport.c) { }
}

struct s : p {
  func f() { }
}

func f(di : d) {
  di.f(get_ci())
}

func blackhole<F>(_ : F) { }

public func g() {
  blackhole({ () -> () in })
}

// CHECK-NO-OPT-DAG: declare dllimport ptr @swift_allocObject(ptr, i32, i32)
// CHECK-NO-OPT-DAG: declare dllimport void @swift_release(ptr)
// CHECK-NO-OPT-DAG: declare dllimport ptr @swift_retain(ptr returned)
// CHECK-NO-OPT-DAG: @"$s9dllexport1pMp" = external dllimport global %swift.protocol
// CHECK-NO-OPT-DAG: declare dllimport swiftcc ptr @"$s9dllexport2ciAA1cCvau"()
// CHECK-NO-OPT-DAG: declare dllimport swiftcc ptr @"$s9dllexport1cCfd"(ptr swiftself)
// CHECK-NO-OPT-DAG: declare dllimport void @swift_deallocClassInstance(ptr, i32, i32)

// CHECK-OPT-DAG: declare dllimport ptr @swift_retain(ptr returned) local_unnamed_addr
// CHECK-OPT-DAG: @"\01__imp_{{_?}}$s9dllexport1pMp" = external externally_initialized constant ptr
// CHECK-OPT-DAG: declare dllimport swiftcc ptr @"$s9dllexport2ciAA1cCvau"()
// CHECK-OPT-DAG: declare dllimport void @swift_deallocClassInstance(ptr, i32, i32)
// CHECK-OPT-DAG: declare dllimport swiftcc ptr @"$s9dllexport1cCfd"(ptr swiftself)
