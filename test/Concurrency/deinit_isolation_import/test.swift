// RUN: %empty-directory(%t/Frameworks)
// RUN: INPUT_DIR=%S/Inputs
// RUN: cp -R $INPUT_DIR/Alpha.framework %t/Frameworks/
// RUN: %empty-directory(%t/Frameworks/Alpha.framework/Modules/Alpha.swiftmodule)
// RUN: %empty-directory(%t/Frameworks/Alpha.framework/Headers/)
// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk) -parse-as-library -module-name Alpha \
// RUN:  -emit-module -o %t/Frameworks/Alpha.framework/Modules/Alpha.swiftmodule/%module-target-triple.swiftmodule \
// RUN:  -enable-objc-interop -disable-objc-attr-requires-foundation-module \
// RUN:  -emit-objc-header -emit-objc-header-path %t/Frameworks/Alpha.framework/Headers/Alpha-Swift.h $INPUT_DIR/Alpha.swift
// RUN: cp -R $INPUT_DIR/Beta.framework %t/Frameworks/
// RUN: %empty-directory(%t/Frameworks/Beta.framework/Headers/)
// RUN: cp $INPUT_DIR/Beta.h %t/Frameworks/Beta.framework/Headers/Beta.h
// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk) -typecheck -verify %s -F %t/Frameworks -F %clang-importer-sdk-path/frameworks
// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk) -parse-as-library -emit-silgen -DSILGEN %s -F %t/Frameworks -F %clang-importer-sdk-path/frameworks | %FileCheck %s
// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk) -parse-as-library -emit-silgen -DSILGEN %s -F %t/Frameworks -F %clang-importer-sdk-path/frameworks | %FileCheck -check-prefix=CHECK-SYMB %s

// REQUIRES: concurrency
// REQUIRES: objc_interop

// Note: intentionally importing Alpha implicitly
import Beta

@globalActor final actor AnotherActor {
  static let shared = AnotherActor()
}

@MainActor
func isolatedFunc() {} // expected-note 3{{calls to global function 'isolatedFunc()' from outside of its actor context are implicitly asynchronous}}

// CHECK-LABEL: @objc @_inheritsConvenienceInitializers class ProbeImplicit_RoundtripNonisolated : RoundtripNonisolated {
// CHECK: @objc deinit
// CHECK: }
// CHECK-SYMB-NOT: ProbeImplicit_RoundtripNonisolated.__isolated_deallocating_deinit
// CHECK-SYMB-NOT: @$s4test34ProbeImplicit_RoundtripNonisolatedCfZ
// CHECK-SYMB: // ProbeImplicit_RoundtripNonisolated.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s4test34ProbeImplicit_RoundtripNonisolatedCfD : $@convention(method) (@owned ProbeImplicit_RoundtripNonisolated) -> () {
class ProbeImplicit_RoundtripNonisolated: RoundtripNonisolated {}

// CHECK-LABEL: @objc @_inheritsConvenienceInitializers class ProbeExplicit_RoundtripNonisolated : RoundtripNonisolated {
// CHECK: @objc deinit
// CHECK: }
// CHECK-SYMB-NOT: ProbeExplicit_RoundtripNonisolated.__isolated_deallocating_deinit
// CHECK-SYMB-NOT: @$s4test34ProbeExplicit_RoundtripNonisolatedCfZ
// CHECK-SYMB: // ProbeExplicit_RoundtripNonisolated.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s4test34ProbeExplicit_RoundtripNonisolatedCfD : $@convention(method) (@owned ProbeExplicit_RoundtripNonisolated) -> () {
class ProbeExplicit_RoundtripNonisolated: RoundtripNonisolated {
    deinit {
#if !SILGEN
        isolatedFunc() // expected-error {{call to main actor-isolated global function 'isolatedFunc()' in a synchronous nonisolated context}}
#endif
    }
}

// CHECK-LABEL: @objc @_inheritsConvenienceInitializers class ProbeImplicit_RoundtripIsolated : RoundtripIsolated {
// Note: Type-checked as isolated, but no @MainActor attribute, because attributes are not added for overriding members
// CHECK: @objc deinit
// CHECK: }
// CHECK-SYMB: ProbeImplicit_RoundtripIsolated.__isolated_deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s4test31ProbeImplicit_RoundtripIsolatedCfZ : $@convention(thin) (@owned ProbeImplicit_RoundtripIsolated) -> () {
// CHECK-SYMB: // ProbeImplicit_RoundtripIsolated.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s4test31ProbeImplicit_RoundtripIsolatedCfD : $@convention(method) (@owned ProbeImplicit_RoundtripIsolated) -> () {
class ProbeImplicit_RoundtripIsolated: RoundtripIsolated {}

// CHECK-LABEL: @objc @_inheritsConvenienceInitializers class ProbeExplicit_RoundtripIsolated : RoundtripIsolated {
// Note: Type-checked as isolated, but no @MainActor attribute, because attributes are not added for overriding members
// CHECK: @objc deinit
// CHECK: }
// CHECK-SYMB: ProbeExplicit_RoundtripIsolated.__isolated_deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s4test31ProbeExplicit_RoundtripIsolatedCfZ : $@convention(thin) (@owned ProbeExplicit_RoundtripIsolated) -> () {
// CHECK-SYMB: // ProbeExplicit_RoundtripIsolated.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s4test31ProbeExplicit_RoundtripIsolatedCfD : $@convention(method) (@owned ProbeExplicit_RoundtripIsolated) -> () {
class ProbeExplicit_RoundtripIsolated: RoundtripIsolated {
    deinit {
        isolatedFunc() // okay
    }
}

// CHECK-LABEL: @objc @_inheritsConvenienceInitializers class ProbeImplicit_BaseNonisolated : BaseNonisolated {
// CHECK: @objc deinit
// CHECK: }
// CHECK-SYMB-NOT: ProbeImplicit_BaseNonisolated.__isolated_deallocating_deinit
// CHECK-SYMB-NOT: @$s4test29ProbeImplicit_BaseNonisolatedCfZ
// CHECK-SYMB: // ProbeImplicit_BaseNonisolated.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s4test29ProbeImplicit_BaseNonisolatedCfD : $@convention(method) (@owned ProbeImplicit_BaseNonisolated) -> () {
class ProbeImplicit_BaseNonisolated: BaseNonisolated {}

// CHECK-LABEL: @objc @_inheritsConvenienceInitializers class ProbeExplicit_BaseNonisolated : BaseNonisolated {
// CHECK: @objc deinit
// CHECK: }
// CHECK-SYMB-NOT: ProbeExplicit_BaseNonisolated.__isolated_deallocating_deinit
// CHECK-SYMB-NOT: @$s4test29ProbeExplicit_BaseNonisolatedCfZ
// CHECK-SYMB: // ProbeExplicit_BaseNonisolated.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s4test29ProbeExplicit_BaseNonisolatedCfD : $@convention(method) (@owned ProbeExplicit_BaseNonisolated) -> () {
class ProbeExplicit_BaseNonisolated: BaseNonisolated {
    deinit {
#if !SILGEN
        isolatedFunc() // expected-error {{call to main actor-isolated global function 'isolatedFunc()' in a synchronous nonisolated context}}
#endif
    }
}

// CHECK-LABEL: @objc @_inheritsConvenienceInitializers class ProbeImplicit_DerivedNonisolated : DerivedNonisolated {
// CHECK: @objc deinit
// CHECK: }
// CHECK-SYMB-NOT: ProbeImplicit_DerivedNonisolated.__isolated_deallocating_deinit
// CHECK-SYMB-NOT: @$s4test32ProbeImplicit_DerivedNonisolatedCfZ
// CHECK-SYMB: // ProbeImplicit_DerivedNonisolated.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s4test32ProbeImplicit_DerivedNonisolatedCfD : $@convention(method) (@owned ProbeImplicit_DerivedNonisolated) -> () {
class ProbeImplicit_DerivedNonisolated: DerivedNonisolated {}

// CHECK-LABEL: @objc @_inheritsConvenienceInitializers class ProbeExplicit_DerivedNonisolated : DerivedNonisolated {
// CHECK: @objc deinit
// CHECK: }
// CHECK-SYMB-NOT: ProbeExplicit_DerivedNonisolated.__isolated_deallocating_deinit
// CHECK-SYMB-NOT: @$s4test32ProbeExplicit_DerivedNonisolatedCfZ
// CHECK-SYMB: // ProbeExplicit_DerivedNonisolated.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s4test32ProbeExplicit_DerivedNonisolatedCfD : $@convention(method) (@owned ProbeExplicit_DerivedNonisolated) -> () {
class ProbeExplicit_DerivedNonisolated: DerivedNonisolated {
    deinit {
#if !SILGEN
        isolatedFunc() // expected-error {{call to main actor-isolated global function 'isolatedFunc()' in a synchronous nonisolated context}}
#endif
    }
}

// CHECK-LABEL: @objc @_inheritsConvenienceInitializers @MainActor class ProbeImplicit_BaseIsolatedClass : BaseIsolatedClass {
// CHECK: @objc deinit
// CHECK: }
// CHECK-SYMB-NOT: ProbeImplicit_BaseIsolatedClass.__isolated_deallocating_deinit
// CHECK-SYMB-NOT: @$s4test31ProbeImplicit_BaseIsolatedClassCfZ
// CHECK-SYMB: // ProbeImplicit_BaseIsolatedClass.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s4test31ProbeImplicit_BaseIsolatedClassCfD : $@convention(method) (@owned ProbeImplicit_BaseIsolatedClass) -> () {
class ProbeImplicit_BaseIsolatedClass: BaseIsolatedClass {}

// CHECK-LABEL: @objc @_inheritsConvenienceInitializers @MainActor class ProbeExplicit_BaseIsolatedClass : BaseIsolatedClass {
// CHECK: @objc @MainActor deinit
// CHECK: }
// CHECK-SYMB: // ProbeExplicit_BaseIsolatedClass.__isolated_deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s4test31ProbeExplicit_BaseIsolatedClassCfZ : $@convention(thin) (@owned ProbeExplicit_BaseIsolatedClass) -> () {
// CHECK-SYMB: // ProbeExplicit_BaseIsolatedClass.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s4test31ProbeExplicit_BaseIsolatedClassCfD : $@convention(method) (@owned ProbeExplicit_BaseIsolatedClass) -> () {
class ProbeExplicit_BaseIsolatedClass: BaseIsolatedClass {
    deinit {
        isolatedFunc() // ok
    }
}

// CHECK-LABEL: @objc @_inheritsConvenienceInitializers @MainActor class ProbeImplicit_DerivedIsolatedClass : DerivedIsolatedClass {
// CHECK: @objc deinit
// CHECK: }
// CHECK-SYMB-NOT: ProbeImplicit_DerivedIsolatedClass.__isolated_deallocating_deinit
// CHECK-SYMB-NOT: @$s4test34ProbeImplicit_DerivedIsolatedClassCfZ
// CHECK-SYMB: // ProbeImplicit_DerivedIsolatedClass.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s4test34ProbeImplicit_DerivedIsolatedClassCfD : $@convention(method) (@owned ProbeImplicit_DerivedIsolatedClass) -> () {
class ProbeImplicit_DerivedIsolatedClass: DerivedIsolatedClass {}

// CHECK-LABEL: @objc @_inheritsConvenienceInitializers @MainActor class ProbeExplicit_DerivedIsolatedClass : DerivedIsolatedClass {
// CHECK: @objc @MainActor deinit
// CHECK: }
// CHECK-SYMB: // ProbeExplicit_DerivedIsolatedClass.__isolated_deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s4test34ProbeExplicit_DerivedIsolatedClassCfZ : $@convention(thin) (@owned ProbeExplicit_DerivedIsolatedClass) -> () {
// CHECK-SYMB: // ProbeExplicit_DerivedIsolatedClass.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s4test34ProbeExplicit_DerivedIsolatedClassCfD : $@convention(method) (@owned ProbeExplicit_DerivedIsolatedClass) -> () {
class ProbeExplicit_DerivedIsolatedClass: DerivedIsolatedClass {
    deinit {
        isolatedFunc() // ok
    }
}

// CHECK-LABEL: @objc @_inheritsConvenienceInitializers class ProbeImplicit_BaseIsolatedDealloc : BaseIsolatedDealloc {
// Note: Type-checked as isolated, but no @MainActor attribute, because attributes are not added for overriding members
// CHECK: @objc deinit
// CHECK: }
// CHECK-SYMB-NOT: ProbeImplicit_BaseIsolatedDealloc.__isolated_deallocating_deinit
// CHECK-SYMB-NOT: @$s4test33ProbeImplicit_BaseIsolatedDeallocCfZ
// CHECK-SYMB: // ProbeImplicit_BaseIsolatedDealloc.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s4test33ProbeImplicit_BaseIsolatedDeallocCfD : $@convention(method) (@owned ProbeImplicit_BaseIsolatedDealloc) -> () {
class ProbeImplicit_BaseIsolatedDealloc: BaseIsolatedDealloc {}

// CHECK-LABEL: @objc @_inheritsConvenienceInitializers class ProbeExplicit_BaseIsolatedDealloc : BaseIsolatedDealloc {
// Note: Type-checked as isolated, but no @MainActor attribute, because attributes are not added for overriding members
// CHECK: @objc deinit
// CHECK: }
// CHECK-SYMB-NOT: ProbeExplicit_BaseIsolatedDealloc.__isolated_deallocating_deinit
// CHECK-SYMB-NOT: @$s4test33ProbeExplicit_BaseIsolatedDeallocCfZ
// CHECK-SYMB: // ProbeExplicit_BaseIsolatedDealloc.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s4test33ProbeExplicit_BaseIsolatedDeallocCfD : $@convention(method) (@owned ProbeExplicit_BaseIsolatedDealloc) -> () {
class ProbeExplicit_BaseIsolatedDealloc: BaseIsolatedDealloc {
    deinit {
        isolatedFunc() // ok
    }
}

#if !SILGEN
class ProbeAnother_BaseIsolatedDealloc: BaseIsolatedDealloc{
    @AnotherActor deinit {} // expected-error {{global actor 'AnotherActor'-isolated deinitializer 'deinit' has different actor isolation from main actor-isolated overridden declaration}}
}
#endif

// CHECK-LABEL: @objc @_inheritsConvenienceInitializers class ProbeImplicit_DerivedIsolatedDealloc : DerivedIsolatedDealloc {
// Note: Type-checked as isolated, but no @MainActor attribute, because attributes are not added for overriding members
// CHECK: @objc deinit
// CHECK: }
// CHECK-SYMB-NOT: ProbeImplicit_DerivedIsolatedDealloc.__isolated_deallocating_deinit
// CHECK-SYMB-NOT: @$s4test36ProbeImplicit_DerivedIsolatedDeallocCfZ
// CHECK-SYMB: // ProbeImplicit_DerivedIsolatedDealloc.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s4test36ProbeImplicit_DerivedIsolatedDeallocCfD : $@convention(method) (@owned ProbeImplicit_DerivedIsolatedDealloc) -> () {
class ProbeImplicit_DerivedIsolatedDealloc: DerivedIsolatedDealloc {}

// CHECK-LABEL: @objc @_inheritsConvenienceInitializers class ProbeExplicit_DerivedIsolatedDealloc : DerivedIsolatedDealloc {
// CHECK: @objc deinit
// CHECK: }
// CHECK-SYMB-NOT: ProbeExplicit_DerivedIsolatedDealloc.__isolated_deallocating_deinit
// CHECK-SYMB-NOT: @$s4test36ProbeExplicit_DerivedIsolatedDeallocCfZ
// CHECK-SYMB: // ProbeExplicit_DerivedIsolatedDealloc.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s4test36ProbeExplicit_DerivedIsolatedDeallocCfD : $@convention(method) (@owned ProbeExplicit_DerivedIsolatedDealloc) -> () {
class ProbeExplicit_DerivedIsolatedDealloc: DerivedIsolatedDealloc {
    deinit {
        isolatedFunc() // ok
    }
}

#if !SILGEN
class ProbeAnother_DerivedIsolatedDealloc: DerivedIsolatedDealloc{
    @AnotherActor deinit {} // expected-error {{global actor 'AnotherActor'-isolated deinitializer 'deinit' has different actor isolation from main actor-isolated overridden declaration}}
}
#endif

// CHECK-LABEL: @objc @_inheritsConvenienceInitializers class ProbeImplicit_DeallocIsolatedFromProtocol : DeallocIsolatedFromProtocol {
// CHECK: @objc deinit
// CHECK: }
// CHECK-SYMB-NOT: ProbeImplicit_DeallocIsolatedFromProtocol.__isolated_deallocating_deinit
// CHECK-SYMB-NOT: @$s4test41ProbeImplicit_DeallocIsolatedFromProtocolCfZ
// CHECK-SYMB: // ProbeImplicit_DeallocIsolatedFromProtocol.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s4test41ProbeImplicit_DeallocIsolatedFromProtocolCfD : $@convention(method) (@owned ProbeImplicit_DeallocIsolatedFromProtocol) -> () {
class ProbeImplicit_DeallocIsolatedFromProtocol: DeallocIsolatedFromProtocol {}

class ProbeAnother_DeallocIsolatedFromProtocol: DeallocIsolatedFromProtocol {
    @AnotherActor deinit {} // ok, base is not isolated
}

// CHECK-LABEL: @objc @_inheritsConvenienceInitializers class ProbeImplicit_DeallocIsolatedFromCategory : DeallocIsolatedFromCategory {
// CHECK: @objc deinit
// CHECK: }
// CHECK-SYMB-NOT: ProbeImplicit_DeallocIsolatedFromCategory.__isolated_deallocating_deinit
// CHECK-SYMB-NOT: @$s4test41ProbeImplicit_DeallocIsolatedFromCategoryCfZ
// CHECK-SYMB: // ProbeImplicit_DeallocIsolatedFromCategory.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s4test41ProbeImplicit_DeallocIsolatedFromCategoryCfD : $@convention(method) (@owned ProbeImplicit_DeallocIsolatedFromCategory) -> () {
class ProbeImplicit_DeallocIsolatedFromCategory: DeallocIsolatedFromCategory {}

class ProbeAnother_DeallocIsolatedFromCategory: DeallocIsolatedFromCategory {
    @AnotherActor deinit {} // ok, base is not isolated
}

// CHECK-LABEL: @objc @_inheritsConvenienceInitializers class ProbeImplicit_DeallocIsolatedFromExtension : DeallocIsolatedFromExtension {
// CHECK: @objc deinit
// CHECK: }
// CHECK-SYMB-NOT: ProbeImplicit_DeallocIsolatedFromExtension.__isolated_deallocating_deinit
// CHECK-SYMB-NOT: @$s4test42ProbeImplicit_DeallocIsolatedFromExtensionCfZ
// CHECK-SYMB: // ProbeImplicit_DeallocIsolatedFromExtension.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s4test42ProbeImplicit_DeallocIsolatedFromExtensionCfD : $@convention(method) (@owned ProbeImplicit_DeallocIsolatedFromExtension) -> () {
class ProbeImplicit_DeallocIsolatedFromExtension: DeallocIsolatedFromExtension {}

class ProbeAnother_DeallocIsolatedFromExtension: DeallocIsolatedFromExtension {
    @AnotherActor deinit {} // ok, base is not isolated
}
