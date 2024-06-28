// RUN: %empty-directory(%t)
// RUN: split-file %s %t

// RUN: %target-build-swift %t/Lib.swift \
// RUN: -module-name=Lib -package-name Pkg \
// RUN: -parse-as-library -emit-module -emit-module-path %t/Lib.swiftmodule -I%t \
// RUN: -Xfrontend -experimental-package-cmo -Xfrontend -experimental-allow-non-resilient-access \
// RUN: -O -wmo -enable-library-evolution
// RUN: %target-sil-opt %t/Lib.swiftmodule -sil-verify-all -o %t/Lib.sil

// RUN: %target-build-swift -module-name=Main -package-name Pkg -I%t -emit-silgen %t/main.swift -o %t/MAIN.sil
// RUN: %target-build-swift -module-name=Main -package-name Pkg -I%t -emit-sil -O %t/main.swift -o %t/Main-opt.sil

// RUN: %FileCheck %s --check-prefix=CHECK < %t/Lib.sil
// RUN: %FileCheck %s --check-prefix=CHECK-MAIN-OPT < %t/Main-opt.sil
// RUN: %FileCheck %s --check-prefix=CHECK-MAIN < %t/MAIN.sil

// REQUIRES: swift_in_compiler

//--- main.swift

import Lib

// CHECK-MAIN-OPT-NOT: function_ref
// CHECK-MAIN-OPT: struct $PubStruct
// CHECK-MAIN-OPT: store {{.*}} to %0 : $*PubStruct
// CHECK-MAIN-OPT: struct $UfiPkgStruct
// CHECK-MAIN-OPT: store {{.*}} to %0 : $*UfiPkgStruct
// CHECK-MAIN-OPT: struct $PkgStruct
// CHECK-MAIN-OPT: store {{.*}} to %0 : $*PkgStruct

// CHECK-MAIN: sil [serialized] [ossa] @$s4Main14inUsePubStructy3Lib0dE0VSiF : $@convention(thin) (Int) -> @out PubStruct {
@inlinable
public func inUsePubStruct(_ arg: Int) -> PubStruct {
  var p = PubStruct(1)
  // CHECK-MAIN: function_ref @$s3Lib9PubStructV3pubSivM : $@yield_once @convention(method) (@inout PubStruct) -> @yields @inout Int
  p.pub += arg
  return p
}

// CHECK-MAIN: sil [ossa] @$s4Main12usePubStructy3Lib0cD0VSiF : $@convention(thin) (Int) -> @out PubStruct {
public func usePubStruct(_ arg: Int) -> PubStruct {
  var p = PubStruct(1)
  // CHECK-MAIN: function_ref @$s3Lib9PubStructVyACSicfC
  // CHECK-MAIN: function_ref @$s3Lib9PubStructV3pubSivM
  p.pub += arg
  return p
}

// CHECK-MAIN: sil [serialized] [ossa] @$s4Main17inUseUfiPkgStructy3Lib0deF0VSiF : $@convention(thin) (Int) -> @out UfiPkgStruct {
@inlinable
package func inUseUfiPkgStruct(_ arg: Int) -> UfiPkgStruct {
  var p = UfiPkgStruct(1)
  // CHECK-MAIN: function_ref @$s3Lib12UfiPkgStructV03ufiC0SivM : $@yield_once @convention(method) (@inout UfiPkgStruct) -> @yields @inout Int
  p.ufiPkg += arg
  return p
}

// CHECK-MAIN: sil package [ossa] @$s4Main15useUfiPkgStructy3Lib0cdE0VSiF : $@convention(thin) (Int) -> @out UfiPkgStruct {
package func useUfiPkgStruct(_ arg: Int) -> UfiPkgStruct {
  var p = UfiPkgStruct(1)
  // CHECK-MAIN: function_ref @$s3Lib12UfiPkgStructVyACSicfC
  // CHECK-MAIN: function_ref @$s3Lib12UfiPkgStructV03ufiC0SivM
  p.ufiPkg += arg
  return p
}

// CHECK-MAIN: sil package [ossa] @$s4Main12usePkgStructy3Lib0cD0VSiF : $@convention(thin) (Int) -> @out PkgStruct {
package func usePkgStruct(_ arg: Int) -> PkgStruct {
  var p = PkgStruct(1)
  // CHECK-MAIN: function_ref @$s3Lib9PkgStructVyACSicfC
  // CHECK-MAIN: function_ref @$s3Lib9PkgStructV3pkgSivM
  p.pkg += arg
  return p
}

//--- Lib.swift

// CHECK: sil package [serialized_for_package] [canonical] @$s3Lib6libPkgyAA0C6StructVSiF : $@convention(thin) (Int) -> @out PkgStruct {
  // CHECK: struct $PkgStruct
  // CHECK: store {{.*}} to %0 : $*PkgStruct

// CHECK: sil [serialized_for_package] [canonical] @$s3Lib6libPubyAA0C6StructVSiF : $@convention(thin) (Int) -> @out PubStruct {
  // CHECK: struct $PubStruct
  // CHECK: store {{.*}} to %0 : $*PubStruct

// CHECK: sil package [serialized_for_package] [canonical] @$s3Lib9libUfiPkgyAA0cD6StructVSiF : $@convention(thin) (Int) -> @out UfiPkgStruct {
  // CHECK: struct $UfiPkgStruct
  // CHECK: store {{.*}} to %0 : $*UfiPkgStruct

/// @inlinable package func inLibUfiPkg(_ arg: Int) -> UfiPkgStruct
// CHECK: sil [serialized] [canonical] @$s3Lib02inA6UfiPkgyAA0cD6StructVSiF : $@convention(thin) (Int) -> @out UfiPkgStruct {
  // CHECK: function_ref @$s3Lib12UfiPkgStructVyACSicfC : $@convention(method) (Int, @thin UfiPkgStruct.Type) -> @out UfiPkgStruct
  // CHECK: function_ref @$s3Lib12UfiPkgStructV03ufiC0SivM : $@yield_once @convention(method) (@inout UfiPkgStruct) -> @yields @inout Int

/// @inlinable func inLibUfiHid(_ arg: Int) -> UfiHidStruct
// CHECK: sil [serialized] [canonical] @$s3Lib02inA6UfiHidyAA0cD6StructVSiF : $@convention(thin) (Int) -> @out UfiHidStruct {
  // CHECK: function_ref @$s3Lib12UfiHidStructVyACSicfC : $@convention(method) (Int, @thin UfiHidStruct.Type) -> @out UfiHidStruct
  // CHECK: function_ref @$s3Lib12UfiHidStructV03ufiC0SivM : $@yield_once @convention(method) (@inout UfiHidStruct) -> @yields @inout Int

/// @inlinable public func inLibPub(_ arg: Int) -> PubStruct
// CHECK: sil [serialized] [canonical] @$s3Lib02inA3PubyAA0C6StructVSiF : $@convention(thin) (Int) -> @out PubStruct {
  // CHECK: function_ref @$s3Lib9PubStructVyACSicfC : $@convention(method) (Int, @thin PubStruct.Type) -> @out PubStruct
  // CHECK: function_ref @$s3Lib9PubStructV3pubSivM : $@yield_once @convention(method) (@inout PubStruct) -> @yields @inout Int

@inlinable
public func inLibPub(_ arg: Int) -> PubStruct {
  var p = PubStruct(1)
  p.pub += arg
  return p
}

public func libPub(_ arg: Int) -> PubStruct {
  var p = PubStruct(1)
  p.pub += arg
  return p
}

@inlinable
package func inLibUfiPkg(_ arg: Int) -> UfiPkgStruct {
  var p = UfiPkgStruct(1)
  p.ufiPkg += arg
  return p
}

package func libUfiPkg(_ arg: Int) -> UfiPkgStruct {
  var p = UfiPkgStruct(1)
  p.ufiPkg += arg
  return p
}

package func libPkg(_ arg: Int) -> PkgStruct {
  var p = PkgStruct(1)
  p.pkg += arg
  return p
}

@inlinable
func inLibUfiHid(_ arg: Int) -> UfiHidStruct {
  var p = UfiHidStruct(1)
  p.ufiHid += arg
  return p
}

func libUfiHid(_ arg: Int) -> UfiHidStruct {
  var p = UfiHidStruct(1)
  p.ufiHid += arg
  return p
}

public struct PubStruct {
  // PubStruct.pub.getter
  // sil [serialized_for_package] [canonical] @$s3Lib9PubStructV3pubSivg : $@convention(method) (@in_guaranteed PubStruct) -> Int {
  public var pub: Int
  public init(_ arg: Int) {
    pub = arg
  }
  public func pubFunc(_ arg: Int) -> Int {
    return arg > 0 ? arg : arg + 11
  }
}

package struct PkgStruct {
  // PkgStruct.pkg.modify
  // sil package [serialized_for_package] [canonical] @$s3Lib9PkgStructV3pkgSivM : $@yield_once @convention(method) (@inout PkgStruct) -> @yields @inout Int {
  package var pkg: Int
  package init(_ arg: Int) {
    pkg = arg
  }
  package func pkgFunc(_ arg: Int) -> Int {
    return arg > 0 ? arg : 11
  }
}

@usableFromInline
package struct UfiPkgStruct {
  // UfiPkgStruct.ufiPkg.getter
  // sil [serialized_for_package] [canonical] @$s3Lib12UfiPkgStructV03ufiC0Sivg : $@convention(method) (@in_guaranteed UfiPkgStruct) -> Int {
  @usableFromInline
  package var ufiPkg: Int
  @usableFromInline
  package init(_ arg: Int) {
    ufiPkg = arg
  }
  @usableFromInline
  package func ufiPkgFunc(_ arg: Int) -> Int {
    return arg > 0 ? arg : 11
  }
}

@usableFromInline
struct UfiHidStruct {
  // UfiHidStruct.ufiHid.setter
  // sil [serialized_for_package] [canonical] @$s3Lib12UfiHidStructV03ufiC0Sivs : $@convention(method) (Int, @inout UfiHidStruct) -> () {
  @usableFromInline
  var ufiHid: Int
  @usableFromInline
  init(_ arg: Int) {
    ufiHid = arg
  }
  @usableFromInline
  func ufiFunc(_ arg: Int) -> Int {
    return arg > 0 ? arg : 11
  }
}
