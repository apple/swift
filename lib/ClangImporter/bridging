// -*- C++ -*-
//===------------------ bridging - C++ and Swift Interop --------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file provides common utilities and annotations that are useful for C++
// codebases that interoperate with Swift.
//
//===----------------------------------------------------------------------===//
#ifndef SWIFT_CLANGIMPORTER_SWIFT_INTEROP_SUPPORT_H
#define SWIFT_CLANGIMPORTER_SWIFT_INTEROP_SUPPORT_H

#ifdef __has_attribute
#define _CXX_INTEROP_HAS_ATTRIBUTE(x) __has_attribute(x)
#else
#define _CXX_INTEROP_HAS_ATTRIBUTE(x) 0
#endif

#if _CXX_INTEROP_HAS_ATTRIBUTE(swift_attr)

/// Specifies that a C++ `class` or `struct` owns and controls the lifetime of all
/// of the objects it references. Such type should not reference any objects whose
/// lifetime is controlled externally. This annotation allows Swift to import methods
/// that return a `class` or `struct` type that is annotated with this macro.
#define SWIFT_SELF_CONTAINED __attribute__((swift_attr("import_owned")))

/// Specifies that a C++ method returns a value that is presumed to contain
/// objects whose lifetime is not dependent on `this` or other parameters passed
/// to the method.
#define SWIFT_RETURNS_INDEPENDENT_VALUE __attribute__((swift_attr("import_unsafe")))

#define _CXX_INTEROP_STRINGIFY(_x) #_x

/// Specifies that a C++ `class` or `struct` is reference-counted using
/// the given `retain` and `release` functions. This annotation lets Swift import
/// such a type as reference counted type in Swift, taking advantage of Swift's
/// automatic reference counting.
///
/// This example shows how to use this macro to let Swift know that
/// a non-copyable reference counted C++ class can be imported as a reference counted type in Swift:
///  ```c++
///    class SWIFT_SHARED_REFERENCE(retainSharedObject, releaseSharedObject)
///    SharedObject : NonCopyable, IntrusiveReferenceCounted<SharedObject> {
///    public:
///      static SharedObject* create();
///      void doSomething();
///    };
///
///    void retainSharedObject(SharedObject *);
///    void releaseSharedObject(SharedObject *);
///  ```
///
///  Then, the Swift programmer would be able to use it in the following manner:
///
///  ```swift
///    let object = SharedObject.create()
///    object.doSomething()
///    // The Swift compiler will release object here.
///  ```
#define SWIFT_SHARED_REFERENCE(_retain, _release)                                \
  __attribute__((swift_attr("import_reference")))                          \
  __attribute__((swift_attr(_CXX_INTEROP_STRINGIFY(retain:_retain))))      \
  __attribute__((swift_attr(_CXX_INTEROP_STRINGIFY(release:_release))))

/// Specifies that a C++ `class` or `struct` is a reference type whose lifetime
/// is presumed to be immortal, i.e. the reference to such object is presumed to
/// always be valid. This annotation lets Swift import such a type as a reference
/// type in Swift.
////
/// This example shows how to use this macro to let Swift know that
/// a non-copyable singleton C++ class can be imported as a reference type in Swift:
///  ```c++
///    class SWIFT_IMMORTAL_REFERENCE
///    LoggerSingleton : NonCopyable {
///    public:
///      static LoggerSingleton &getInstance();
///      void log(int x);
///    };
///  ```
///
///  Then, the Swift programmer would be able to use it in the following manner:
///
///  ```swift
///    let logger = LoggerSingleton.getInstance()
///    logger.log(123)
///  ```
#define SWIFT_IMMORTAL_REFERENCE                                                \
  __attribute__((swift_attr("import_reference")))                         \
  __attribute__((swift_attr(_CXX_INTEROP_STRINGIFY(retain:immortal))))    \
  __attribute__((swift_attr(_CXX_INTEROP_STRINGIFY(release:immortal))))

/// Specifies that a C++ `class` or `struct` is a reference type whose lifetime
/// is not managed automatically. The programmer must validate that any reference
/// to such object is valid themselves. This annotation lets Swift import such a type as a reference type in Swift.
#define SWIFT_UNSAFE_REFERENCE                                                  \
  __attribute__((swift_attr("import_reference")))                         \
  __attribute__((swift_attr(_CXX_INTEROP_STRINGIFY(retain:immortal))))    \
  __attribute__((swift_attr(_CXX_INTEROP_STRINGIFY(release:immortal))))

/// Specifies a name that will be used in Swift for this declaration instead of its original name.
#define SWIFT_NAME(_name) __attribute__((swift_name(#_name)))

/// Specifies that a specific C++ `class` or `struct` conforms to a
/// a specific Swift protocol.
///
/// This example shows how to use this macro to conform a class template to a Swift protocol:
///  ```
///    template<class T>
///    class SWIFT_CONFORMS_TO_PROTOCOL(SwiftModule.ProtocolName)
///    CustomClass {};
///  ```
#define SWIFT_CONFORMS_TO_PROTOCOL(_moduleName_protocolName) \
  __attribute__((swift_attr(_CXX_INTEROP_STRINGIFY(conforms_to:_moduleName_protocolName))))

/// Specifies that a specific C++ method should be imported as a computed
/// property. If this macro is specified on a getter, a getter will be
/// synthesized. If this macro is specified on a setter, both a getter and
/// setter will be synthesized.
///
/// For example:
///  ```
///    int getX() SWIFT_COMPUTED_PROPERTY;
///  ```
/// Will be imported as `var x: CInt {...}`.
#define SWIFT_COMPUTED_PROPERTY \
  __attribute__((swift_attr("import_computed_property")))

/// Specifies that a specific **constant** C++ member function should be imported as
/// `mutating` Swift method. This annotation should be added to constant C++ member functions
/// that mutate `mutable` fields in a C++ object, to let Swift know that this function is still mutating
/// and thus that it should become a `mutating` method in Swift.
#define SWIFT_MUTATING \
  __attribute__((swift_attr("mutating")))

#else  // #if _CXX_INTEROP_HAS_ATTRIBUTE(swift_attr)

// Empty defines for compilers that don't support `attribute(swift_attr)`.
#define SWIFT_SELF_CONTAINED
#define SWIFT_RETURNS_INDEPENDENT_VALUE
#define SWIFT_SHARED_REFERENCE(_retain, _release)
#define SWIFT_IMMORTAL_REFERENCE
#define SWIFT_UNSAFE_REFERENCE
#define SWIFT_NAME(_name)
#define SWIFT_CONFORMS_TO_PROTOCOL(_moduleName_protocolName)
#define SWIFT_COMPUTED_PROPERTY
#define SWIFT_MUTATING

#endif // #if _CXX_INTEROP_HAS_ATTRIBUTE(swift_attr)

#undef _CXX_INTEROP_HAS_ATTRIBUTE

#endif // SWIFT_CLANGIMPORTER_SWIFT_INTEROP_SUPPORT_H
