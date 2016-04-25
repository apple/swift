// RUN: rm -rf %t && mkdir -p %t
// RUN: %target-build-swift %S/Inputs/TypeLowering.swift -parse-as-library -emit-module -emit-library -module-name TypeLowering -Xfrontend -enable-reflection-metadata -Xfrontend -enable-reflection-names -o %t/libTypesToReflect.%target-dylib-extension
// RUN: %target-swift-reflection-dump -binary-filename %t/libTypesToReflect.%target-dylib-extension -binary-filename %platform-module-dir/libswiftCore.%target-dylib-extension -dump-type-lowering < %s | FileCheck %s

// REQUIRES: CPU=x86_64

V12TypeLowering11BasicStruct
// CHECK:      (struct TypeLowering.BasicStruct)

// CHECK-NEXT: (struct size=16 alignment=4 stride=16 num_extra_inhabitants=0
// CHECK-NEXT:   (field name=i1 offset=0
// CHECK-NEXT:     (struct size=1 alignment=1 stride=1 num_extra_inhabitants=0
// CHECK-NEXT:       (field offset=0
// CHECK-NEXT:         (builtin size=1 alignment=1 stride=1 num_extra_inhabitants=0))))
// CHECK-NEXT:   (field name=i2 offset=2
// CHECK-NEXT:     (struct size=2 alignment=2 stride=2 num_extra_inhabitants=0
// CHECK-NEXT:       (field offset=0
// CHECK-NEXT:         (builtin size=2 alignment=2 stride=2 num_extra_inhabitants=0))))
// CHECK-NEXT:   (field name=i3 offset=4
// CHECK-NEXT:     (struct size=4 alignment=4 stride=4 num_extra_inhabitants=0
// CHECK-NEXT:       (field offset=0
// CHECK-NEXT:         (builtin size=4 alignment=4 stride=4 num_extra_inhabitants=0))))
// CHECK-NEXT:   (field name=bi1 offset=8
// CHECK-NEXT:     (struct size=1 alignment=1 stride=1 num_extra_inhabitants=0
// CHECK-NEXT:       (field name=value offset=0
// CHECK-NEXT:         (struct size=1 alignment=1 stride=1 num_extra_inhabitants=0
// CHECK-NEXT:           (field offset=0
// CHECK-NEXT:             (builtin size=1 alignment=1 stride=1 num_extra_inhabitants=0))))))
// CHECK-NEXT:   (field name=bi2 offset=10
// CHECK-NEXT:     (struct size=2 alignment=2 stride=2 num_extra_inhabitants=0
// CHECK-NEXT:       (field name=value offset=0
// CHECK-NEXT:         (struct size=2 alignment=2 stride=2 num_extra_inhabitants=0
// CHECK-NEXT:           (field offset=0
// CHECK-NEXT:             (builtin size=2 alignment=2 stride=2 num_extra_inhabitants=0))))))
// CHECK-NEXT:   (field name=bi3 offset=12
// CHECK-NEXT:     (struct size=4 alignment=4 stride=4 num_extra_inhabitants=0
// CHECK-NEXT:       (field name=value offset=0
// CHECK-NEXT:         (struct size=4 alignment=4 stride=4 num_extra_inhabitants=0
// CHECK-NEXT:           (field offset=0
// CHECK-NEXT:             (builtin size=4 alignment=4 stride=4 num_extra_inhabitants=0)))))))


V12TypeLowering15AssocTypeStruct
// CHECK:      (struct TypeLowering.AssocTypeStruct)
// CHECK-NEXT: (struct size=7 alignment=2 stride=8 num_extra_inhabitants=0
// CHECK-NEXT:   (field name=t offset=0
// CHECK-NEXT:     (struct size=7 alignment=2 stride=8 num_extra_inhabitants=0
// CHECK-NEXT:       (field name=a offset=0
// CHECK-NEXT:         (struct size=2 alignment=2 stride=2 num_extra_inhabitants=0
// CHECK-NEXT:           (field name=value offset=0
// CHECK-NEXT:             (struct size=2 alignment=2 stride=2 num_extra_inhabitants=0
// CHECK-NEXT:               (field offset=0
// CHECK-NEXT:                 (builtin size=2 alignment=2 stride=2 num_extra_inhabitants=0))))))
// CHECK-NEXT:       (field name=b offset=2
// CHECK-NEXT:         (struct size=1 alignment=1 stride=1 num_extra_inhabitants=0
// CHECK-NEXT:           (field name=value offset=0
// CHECK-NEXT:             (struct size=1 alignment=1 stride=1 num_extra_inhabitants=0
// CHECK-NEXT:               (field offset=0
// CHECK-NEXT:                 (builtin size=1 alignment=1 stride=1 num_extra_inhabitants=0))))))
// CHECK-NEXT:       (field name=c offset=4
// CHECK-NEXT:         (tuple size=3 alignment=2 stride=4 num_extra_inhabitants=0
// CHECK-NEXT:           (field offset=0
// CHECK-NEXT:             (struct size=2 alignment=2 stride=2 num_extra_inhabitants=0
// CHECK-NEXT:               (field name=value offset=0
// CHECK-NEXT:                 (struct size=2 alignment=2 stride=2 num_extra_inhabitants=0
// CHECK-NEXT:                   (field offset=0
// CHECK-NEXT:                     (builtin size=2 alignment=2 stride=2 num_extra_inhabitants=0))))))
// CHECK-NEXT:           (field offset=2
// CHECK-NEXT:             (struct size=1 alignment=1 stride=1 num_extra_inhabitants=0
// CHECK-NEXT:               (field name=value offset=0
// CHECK-NEXT:                 (struct size=1 alignment=1 stride=1 num_extra_inhabitants=0
// CHECK-NEXT:                   (field offset=0
// CHECK-NEXT:                     (builtin size=1 alignment=1 stride=1 num_extra_inhabitants=0)))))))))))

TGV12TypeLowering3BoxVs5Int16_Vs5Int32_
// CHECK-NEXT: (tuple
// CHECK-NEXT:   (bound_generic_struct TypeLowering.Box
// CHECK-NEXT:     (struct Swift.Int16))
// CHECK-NEXT:   (struct Swift.Int32))

// CHECK-NEXT: (tuple size=8 alignment=4 stride=8 num_extra_inhabitants=0
// CHECK-NEXT:   (field offset=0
// CHECK-NEXT:     (struct size=2 alignment=2 stride=2 num_extra_inhabitants=0
// CHECK-NEXT:       (field name=value offset=0
// CHECK-NEXT:         (struct size=2 alignment=2 stride=2 num_extra_inhabitants=0
// CHECK-NEXT:           (field offset=0
// CHECK-NEXT:             (builtin size=2 alignment=2 stride=2 num_extra_inhabitants=0))))))
// CHECK-NEXT:   (field offset=4
// CHECK-NEXT:     (struct size=4 alignment=4 stride=4 num_extra_inhabitants=0
// CHECK-NEXT:       (field offset=0
// CHECK-NEXT:         (builtin size=4 alignment=4 stride=4 num_extra_inhabitants=0)))))

V12TypeLowering15ReferenceStruct
// CHECK:      (struct TypeLowering.ReferenceStruct)

// CHECK-NEXT: (struct size=40 alignment=8 stride=40 num_extra_inhabitants=0
// CHECK-NEXT:   (field name=strongRef offset=0
// CHECK-NEXT:     (reference kind=strong refcounting=native))
// CHECK-NEXT:   (field name=strongOptionalRef offset=8
// CHECK-NEXT:     (reference kind=strong refcounting=native))
// CHECK-NEXT:   (field name=unownedRef offset=16
// CHECK-NEXT:     (reference kind=unowned refcounting=native))
// CHECK-NEXT:   (field name=weakRef offset=24
// CHECK-NEXT:     (reference kind=weak refcounting=native))
// CHECK-NEXT:   (field name=unmanagedRef offset=32
// CHECK-NEXT:     (reference kind=unmanaged refcounting=native)))

V12TypeLowering14FunctionStruct
// CHECK:      (struct TypeLowering.FunctionStruct)

// CHECK-NEXT: (struct size=32 alignment=8 stride=32 num_extra_inhabitants=0
// CHECK-NEXT:   (field name=thickFunction offset=0
// CHECK-NEXT:     (thick_function size=16 alignment=8 stride=16 num_extra_inhabitants=0
// CHECK-NEXT:       (field name=function offset=0
// CHECK-NEXT:         (builtin size=8 alignment=8 stride=8 num_extra_inhabitants=1))
// CHECK-NEXT:       (field name=context offset=8
// CHECK-NEXT:         (reference kind=strong refcounting=native))))
// CHECK-NEXT:   (field name=thinFunction offset=16
// CHECK-NEXT:     (builtin size=8 alignment=8 stride=8 num_extra_inhabitants=1))
// CHECK-NEXT:   (field name=cFunction offset=24
// CHECK-NEXT:     (builtin size=8 alignment=8 stride=8 num_extra_inhabitants=1)))

Bo
// CHECK:      (builtin Builtin.NativeObject)
// CHECK-NEXT: (reference kind=strong refcounting=native)

BO
// CHECK:      (builtin Builtin.UnknownObject)
// CHECK-NEXT: (reference kind=strong refcounting=unknown)
