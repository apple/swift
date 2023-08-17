// RUN: %empty-directory(%t)
// RUN: split-file %s %t
// RUN: %target-swift-frontend %t/test.swift -I %t/Inputs -typecheck -enable-library-evolution -enable-experimental-cxx-interop -verify

// REQUIRES: objc_interop

//--- Inputs/module.modulemap
module ObjCxxModule {
    header "header.h"
    requires cplusplus
}

//--- Inputs/header.h

#import <Foundation/Foundation.h>

class CxxStruct {
public:
    int x; int y;

    void method() const;
};

@interface ObjCClass: NSObject

- (void)myTestMethod;
- (int *)returnsIntPtr;

- (CxxStruct)testMethodReturnsCxxStruct;
- (void)testMethodTakesCxxStructPtr: (CxxStruct * _Nullable) ptr;
+ (ObjCClass * _Nonnull)getInstance;

@property int intProp;
@property(copy) ObjCClass * _Nonnull objcClassProp;
@property CxxStruct * structPtrProp;

@end

using ObjCClassTypealias = ObjCClass * _Nonnull;

@protocol ObjCProto

- (void)testProto;

@end

using QualIDTypeAlias = id<ObjCProto>;

using BuiltinIntTypealis = int;

//--- test.swift

import ObjCxxModule

// ok
public func usesObjCClass() -> ObjCClass {
    return ObjCClass.getInstance()
}

public func usesObjCClassTypealias(_ x: ObjCClassTypealias) {
}

public func usesObjCProto(_ x: ObjCProto) {
}

public func usesQualIDTypeAlias(_ x: QualIDTypeAlias) {
}

public func usesBuiltinIntTypealis() -> BuiltinIntTypealis {
    return 21
}

@inlinable
public func publicFuncPublicBody() {
    let i = ObjCClass.getInstance()
    i.myTestMethod()
    i.returnsIntPtr()
    let _ = i.intProp
    let _ = i.objcClassProp
    // expected-error@+1 {{instance method 'testMethodReturnsCxxStruct()' cannot be used in an '@inlinable' function because C++ APIs from imported module 'ObjCxxModule' do not support library evolution}}
    i.testMethodReturnsCxxStruct()
    // expected-error@+1 {{instance method 'testMethodTakesCxxStructPtr' cannot be used in an '@inlinable' function because C++ APIs from imported module 'ObjCxxModule' do not support library evolution}}
    i.testMethodTakesCxxStructPtr(nil)
    // expected-error@+1 {{property 'structPtrProp' cannot be used in an '@inlinable' function because C++ APIs from imported module 'ObjCxxModule' do not support library evolution}}
    let _ = i.structPtrProp
}

// expected-error@+1 {{cannot use struct 'CxxStruct' here; C++ types from imported module 'ObjCxxModule' do not support library evolution}}
public func usesCxxStruct(_ x: CxxStruct) {
}
