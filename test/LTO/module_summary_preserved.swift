// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -emit-sib -emit-module-summary-path %t/preserved.swiftmodule.summary -module-name preserved -Xllvm -module-summary-embed-debug-name %s
// RUN: %swift-module-summary-test --to-yaml %t/preserved.swiftmodule.summary -o %t/preserved.summary.yaml
// RUN: cat %t/preserved.summary.yaml | %FileCheck %s -check-prefix REPLACABLE
// REPLACABLE:      909315153062346157:
// REPLACABLE-NEXT:    name:            '$s9preserved12replaceable1SiyF'
// REPLACABLE-NEXT:    guid:            909315153062346157
// REPLACABLE-NEXT:    live:            false
// REPLACABLE-NEXT:    preserved:       true

// REPLACABLE:      3380283816534000009:
// REPLACABLE-NEXT:    name:            '$s9preserved14replaceable1_rSiyF'
// REPLACABLE-NEXT:    guid:            3380283816534000009
// REPLACABLE-NEXT:    live:            false
// REPLACABLE-NEXT:    preserved:       false

dynamic func replaceable1() -> Int {
   return 0
}

@_dynamicReplacement(for: replaceable1())
func replaceable1_r() -> Int {
  return 3
}


// RUN: cat %t/preserved.summary.yaml | %FileCheck %s -check-prefix CDECL 
// CDECL:      401177591854398425:
// CDECL-NEXT:    name:            callableFromC2
// CDECL-NEXT:    guid:            401177591854398425
// CDECL-NEXT:    live:            false
// CDECL-NEXT:    preserved:       true
// CDECL-NEXT:    calls:           []
// CDECL:      2609850307322683057:
// CDECL-NEXT:    name:            callableFromC1
// CDECL-NEXT:    guid:            2609850307322683057
// CDECL-NEXT:    live:            false
// CDECL-NEXT:    preserved:       true
@_cdecl("callableFromC1")
func callableFromC1(x: Int) -> Int {
  return 1
}

@_silgen_name("callableFromC2")
func callableFromC2(x: Int) -> Int {
  return 2
}

// RUN: if [ %target-runtime == "objc" ]; then cat %t/preserved.summary.yaml | %FileCheck %s -check-prefix OBJC; fi
// OBJC:      3149498140227613915:
// OBJC-NEXT:    name:            '$s9preserved1AC11objcMethod1yyFTo'
// OBJC-NEXT:    guid:            3149498140227613915
// OBJC-NEXT:    live:            false
// OBJC-NEXT:    preserved:       true

#if canImport(ObjectiveC)
import Foundation

class A: NSObject {
  @objc func objcMethod1() {}
}
#endif
