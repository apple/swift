// RUN: %target-swift-ide-test -print-module -module-to-print=Templates -I %S/Inputs -source-filename=x -enable-cxx-interop | %FileCheck %s

// CHECK-NOT: extension
// CHECK: extension TemplatesNS1 {
// CHECK:   static func basicFunctionTemplate<T>(_: T) -> UnsafePointer<CChar>!
// CHECK:   struct __CxxTemplateInstN12TemplatesNS118BasicClassTemplateIcEE {
// CHECK:     init()
// CHECK:     mutating func basicMember() -> UnsafePointer<CChar>!
// CHECK:   }
// CHECK:   typealias BasicClassTemplateChar = TemplatesNS1.__CxxTemplateInstN12TemplatesNS118BasicClassTemplateIcEE
// CHECK: }
// CHECK-NOT: extension


// CHECK: extension TemplatesNS1.TemplatesNS2 {
// CHECK:   static func forwardDeclaredFunctionTemplate<T>(_: T) -> UnsafePointer<CChar>!
// CHECK:   struct __CxxTemplateInstN12TemplatesNS112TemplatesNS228ForwardDeclaredClassTemplateIcEE {
// CHECK:     init()
// CHECK:     mutating func basicMember() -> UnsafePointer<CChar>!
// CHECK:   }
// CHECK:   static func forwardDeclaredFunctionTemplateOutOfLine<T>(_: T) -> UnsafePointer<CChar>!
// CHECK:   struct __CxxTemplateInstN12TemplatesNS112TemplatesNS237ForwardDeclaredClassTemplateOutOfLineIcEE {
// CHECK:     init()
// CHECK:     mutating func basicMember() -> UnsafePointer<CChar>!
// CHECK:   }
// CHECK: }
// CHECK-NOT: extension

// CHECK: extension TemplatesNS1 {
// CHECK:   typealias ForwardDeclaredClassTemplateChar = TemplatesNS1.TemplatesNS2.__CxxTemplateInstN12TemplatesNS112TemplatesNS228ForwardDeclaredClassTemplateIcEE
// CHECK: }
// CHECK: typealias ForwardDeclaredClassTemplateOutOfLineChar = TemplatesNS1.TemplatesNS2.__CxxTemplateInstN12TemplatesNS112TemplatesNS237ForwardDeclaredClassTemplateOutOfLineIcEE

// CHECK: extension TemplatesNS1.TemplatesNS2 {
// CHECK:   typealias BasicClassTemplateChar = TemplatesNS1.TemplatesNS3.__CxxTemplateInstN12TemplatesNS112TemplatesNS318BasicClassTemplateIcEE
// CHECK:   static func takesClassTemplateFromSibling(_: TemplatesNS1.TemplatesNS2.BasicClassTemplateChar) -> UnsafePointer<CChar>!
// CHECK: }

// CHECK: extension TemplatesNS4 {
// CHECK:   struct __CxxTemplateInstN12TemplatesNS417HasSpecializationIcEE {
// CHECK:     init()
// CHECK:   }
// CHECK:   struct __CxxTemplateInstN12TemplatesNS417HasSpecializationIiEE {
// CHECK:     init()
// CHECK:   }
// CHECK: }

// CHECK: extension TemplatesNS1 {
// CHECK:   typealias UseTemplate = TemplatesNS4.__CxxTemplateInstN12TemplatesNS417HasSpecializationIcEE
// CHECK: }
