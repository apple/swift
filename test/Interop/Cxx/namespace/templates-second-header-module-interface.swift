// RUN: %target-swift-ide-test -print-module -module-to-print=TemplatesSecondHeader -I %S/Inputs -source-filename=x -enable-cxx-interop | %FileCheck %s

// CHECK: extension TemplatesNS1 {
// CHECK:   static func basicFunctionTemplateDefinedInDefs<T>(_: T) -> UnsafePointer<CChar>!
// CHECK:   struct __CxxTemplateInstN12TemplatesNS131BasicClassTemplateDefinedInDefsIcEE {
// CHECK:     init()
// CHECK:     mutating func basicMember() -> UnsafePointer<CChar>!
// CHECK:   }
// CHECK: }

// CHECK: typealias BasicClassTemplateDefinedInDefsChar = TemplatesNS1.__CxxTemplateInstN12TemplatesNS131BasicClassTemplateDefinedInDefsIcEE
