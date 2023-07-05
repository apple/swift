// RUN: %target-swift-emit-ir -I %S/Inputs -enable-experimental-cxx-interop %s -Xcc -fignore-exceptions | %FileCheck %s

import Reference

public func getCxxRef() -> UnsafeMutablePointer<CInt> {
  return getStaticIntRef()
}

// CHECK: define {{(protected |dllexport )?}}swiftcc ptr @"$s4main9getCxxRefSpys5Int32VGyF"()
// CHECK: call ptr @{{_Z15getStaticIntRefv|"\?getStaticIntRef@@YAAEAHXZ"}}()

public func getConstCxxRef() -> UnsafePointer<CInt> {
  return getConstStaticIntRef()
}

// CHECK: define {{(protected |dllexport )?}}swiftcc ptr @"$s4main14getConstCxxRefSPys5Int32VGyF"()
// CHECK: call ptr @{{_Z20getConstStaticIntRefv|"\?getConstStaticIntRef@@YAAEBHXZ"}}()

public func getCxxRvalueRef() -> UnsafeMutablePointer<CInt> {
  return getStaticIntRvalueRef()
}

// CHECK: define {{(protected |dllexport )?}}swiftcc ptr @"$s4main15getCxxRvalueRefSpys5Int32VGyF"()
// CHECK: call ptr @{{_Z21getStaticIntRvalueRefv|"\?getStaticIntRvalueRef@@YA\$\$QEAHXZ"}}()

public func getConstCxxRvalueRef() -> UnsafePointer<CInt> {
  return getConstStaticIntRvalueRef()
}

// CHECK: define {{(protected |dllexport )?}}swiftcc ptr @"$s4main20getConstCxxRvalueRefSPys5Int32VGyF"()
// CHECK: call ptr @{{_Z26getConstStaticIntRvalueRefv|"\?getConstStaticIntRvalueRef@@YA\$\$QEBHXZ"}}()

public func setCxxRef() {
  var val: CInt = 21
  setStaticIntRef(&val)
}

// CHECK: define {{(protected |dllexport )?}}swiftcc void @"$s4main9setCxxRefyyF"()
// CHECK: call void @{{_Z15setStaticIntRefRi|"\?setStaticIntRef@@YAXAEAH@Z"}}(ptr %{{.*}})

public func setCxxConstRef() {
  let val: CInt = 21
  setConstStaticIntRef(val)
}

// CHECK: define {{(protected |dllexport )?}}swiftcc void @"$s4main14setCxxConstRefyyF"()
// CHECK: call void @{{_Z20setConstStaticIntRefRKi|"\?setConstStaticIntRef@@YAXAEBH@Z"}}(ptr %{{.*}})
