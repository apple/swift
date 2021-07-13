// RUN: %target-swift-ide-test -print-module -module-to-print=DefaultedTemplateTypeParameter -I %S/Inputs -source-filename=x -enable-cxx-interop | %FileCheck %s

// CHECK: struct X {
// CHECK:   init<T>(_: T)
// CHECK:   init()
// CHECK: }

// CHECK: func defaultedTemplateTypeParam()
// CHECK: func defaultedTemplateTypeParamUsedInArgs<T>(_: T)
// CHECK: func defaultedTemplateTypeParamUsedInReturn<T>() -> T
// CHECK: func defaultedTemplateTypeParamAndDefaultedParam<T>(_: T)
// CHECK: func functionTemplateWithDefaultedParam<T>(_: T)
// CHECK: func defaultedTemplateTypeParamUsedInSignatureAndUnrealtedParam<T>(_: Int32, _: T)
// CHECK: func defaultedTemplateTypeParamAndUnrealtedParam(_: Int32)
// CHECK: func overloadedDefaultedTemplate<T>(_: T)
// CHECK: func overloadedDefaultedTemplate(_: Int32)
// CHECK: func defaultedTemplateReferenceTypeParam<T>(_ t: UnsafeMutablePointer<T>)
// The following types aren't imported correctly, but that does not have to do
// with the fact that the template type paramaters are defaulted.
// CHECK: func defaultedTemplatePointerTypeParam<T>(_ t: OpaquePointer!)
// CHECK: func defaultedTemplatePointerReferenceTypeParam<T>(_ t: UnsafeMutablePointer<OpaquePointer?>)
// CHECK: func defaultedTemplatePointerPointerTypeParam<T>(_ t: UnsafeMutablePointer<OpaquePointer?>!)
