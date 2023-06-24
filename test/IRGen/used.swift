// RUN: %target-swift-frontend -enable-experimental-feature SymbolLinkageMarkers -primary-file %s    -emit-sil | %FileCheck %s --check-prefix=SIL
// RUN: %target-swift-frontend -enable-experimental-feature SymbolLinkageMarkers -primary-file %s -O -emit-sil | %FileCheck %s --check-prefix=SIL
// RUN: %target-swift-frontend %use_no_opaque_pointers -enable-experimental-feature SymbolLinkageMarkers -primary-file %s    -emit-ir  | %FileCheck %s --check-prefix=IR
// RUN: %target-swift-frontend %use_no_opaque_pointers -enable-experimental-feature SymbolLinkageMarkers -primary-file %s -O -emit-ir  | %FileCheck %s --check-prefix=IR
// RUN: %target-swift-frontend -enable-experimental-feature SymbolLinkageMarkers -primary-file %s    -emit-ir
// RUN: %target-swift-frontend -enable-experimental-feature SymbolLinkageMarkers -primary-file %s -O -emit-ir
// RUN: %target-swift-frontend -enable-experimental-feature SymbolLinkageMarkers -primary-file %s    -emit-sil -parse-as-library | %FileCheck %s --check-prefix=SIL
// RUN: %target-swift-frontend -enable-experimental-feature SymbolLinkageMarkers -primary-file %s -O -emit-sil -parse-as-library | %FileCheck %s --check-prefix=SIL
// RUN: %target-swift-frontend %use_no_opaque_pointers -enable-experimental-feature SymbolLinkageMarkers -primary-file %s    -emit-ir  -parse-as-library | %FileCheck %s --check-prefix=IR
// RUN: %target-swift-frontend %use_no_opaque_pointers -enable-experimental-feature SymbolLinkageMarkers -primary-file %s -O -emit-ir  -parse-as-library | %FileCheck %s --check-prefix=IR
// RUN: %target-swift-frontend -enable-experimental-feature SymbolLinkageMarkers -primary-file %s    -emit-ir  -parse-as-library
// RUN: %target-swift-frontend -enable-experimental-feature SymbolLinkageMarkers -primary-file %s -O -emit-ir  -parse-as-library

// REQUIRES: swift_in_compiler

@_used var g0: Int = 1
@_used var g1: (Int, Int) = (42, 43)
@_used var g2: Bool = true
@_used func foo() {}

// SIL: @_used @_hasStorage @_hasInitialValue var g0: Int { get set }
// SIL: @_used @_hasStorage @_hasInitialValue var g1: (Int, Int) { get set }
// SIL: @_used @_hasStorage @_hasInitialValue var g2: Bool { get set }
// SIL: @_used func foo()

// SIL: sil_global hidden @$s4used2g0Sivp : $Int
// SIL: sil_global hidden @$s4used2g1Si_Sitvp : $(Int, Int)
// SIL: sil_global hidden @$s4used2g2Sbvp : $Bool

// SIL: sil hidden [used] @$s4used3fooyyF : $@convention(thin)

// IR:      @llvm{{(\.compiler)?}}.used = appending global [{{.*}} x i8*] [
// IR-SAME: i8* bitcast (%TSi* @"$s4used2g0Sivp" to i8*)
// IR-SAME: i8* bitcast (<{ %TSi, %TSi }>* @"$s4used2g1Si_Sitvp" to i8*)
// IR-SAME: i8* bitcast (%TSb* @"$s4used2g2Sbvp" to i8*)
// IR-SAME: i8* bitcast (void ()* @"$s4used3fooyyF" to i8*)
