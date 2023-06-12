[** ‼️ The official C++ interoperability documentation is live at Swift.org and provides an up-to-date guide for mixing Swift and C++ ‼️ **](https://www.swift.org/documentation/cxx-interop/)

# C++ Interop User Manual

The following document explains how C++ APIs are imported into Swift and is targeted at users of C++ interoperability. Hopefully this document will help you understand why the compiler cannot import various APIs and help you update these APIs to be useable from Swift. First, the document will lay out some API patterns and definitions, then it will discuss how the Swift compiler decides if an API should be usable in Swift. 

## Reference Types

Reference types have reference semantics and object identity. A reference type is a pointer (or “reference”) to some object which means there is a layer of indirection. When a reference type is copied, the pointer’s value is copied rather than the object’s storage. This means reference types can be used to represent non-copyable types in C++. Any C++ APIs that use reference types must have at least one layer of indirection to the type (a pointer or reference). Currently reference types must be immortal (never deallocated) or have manually managed lifetimes. You can specify a type has reference semantics by using the `import_reference` swift attribute.

### Importing custom reference counted types into Swift

C++ types that have their own reference count and custom retain/release operations
can be bridged over as reference types into Swift. Swift's automatic reference counting (ARC)
will automatically retain and release them using their C++ reference counting operations.

The `import_reference` swift attribute can be applied with the appropriate `retain` and `release` attributes
to import a type with such semantics as a reference type into Swift.
The example below illustrates how this can be done for the C++ `MyReferenceObject` class:

```c++
#define SWIFT_CXX_REF_MYREFOBJECT   \
    __attribute__((swift_attr("import_reference")))   \
    __attribute__((swift_attr("retain:incRef")))   \
    __attribute__((swift_attr("release:decRef")))

class SWIFT_CXX_REF_MYREFOBJECT MyReferenceObject {
private:
  int referenceCount; // the custom reference count.
};

/// Increment the reference count for the given object.
void incRef(MyReferenceObject *object);

/// Decrement the reference count for the given object. When it reaches zero,
/// the object is deallocated.
void decRef(MyReferenceObject *object);
```

## Owned Types

Owned types “own” some storage which can be copied and destroyed. An owned type must be copyable and destructible. The copy constructor must copy any storage that is owned by the type and the destructor must destroy that storage. Copies and destroys must balance out and these operations must not have side effects. Examples of owned types include `std::vector` and `std::string`. The Swift compiler will assume that any types which do not contain pointers are owned types. You can also specify a type is an owned type by using the `import_owned` swift attribute.

## Iterator Types

An iterator represents a point in a range. Iterator types must provide a comparison operator and increment operator (increment or ++ operators are imported as a member called successor). Iterators can be returned by `begin` and `end` methods to form a range which will automatically be imported as a Sequence in Swift. Iterators can often be inferred by the compiler using the C++ iterator traits, but you can also specify a type is an iterator by using the `import_iterator` swift attribute.

## Trivial Types

Trivial types can be copied by copying the bits of a value of the trivial type and do not need any special destruction logic. Trivial types are inferred by the compiler and cannot be specified using an attribute. Trivial types own their storage, so rules below that apply to owned types also apply to trivial types (specifically regarding projections). Pointers are not trivial types. When Objective-C++ mode is enabled, C++ types that hold Objective-C classes are still considered trivial, even though they technically violate the above contract.

## Unsafe Types

All types which do not fall into one of the above categories are considered unsafe. Any unsafe API, including unsafe types that are copyable and destructible may be imported using the `import_unsafe` swift attribute. There are two kinds of unsafe types: **unsafe pointer types** and **un-importable types**.

**Unsafe Pointer Types:** are types which contain an un-owned pointer. This type is assumed to represent an unsafe memory projection when used as a return type for the method of an owned type.

**Un-importable Types:** are types that are not copyable or destructible. These types cannot be represented in Swift, so they cannot be imported (even if they are marked with the `import_unsafe` attribute).

## API rules

The Swift compiler enforces certain API rules, not to ensure a completely safe C++ API interface, but to prevent especially unsafe patterns that will likely lead to bugs. Many of these unsafe patterns stem from the subtly different lifetime semantics in C++ and Swift and aim to prevent memory projections of owned types. The currently enforced rules are as follows:

* Un-importable types are not allowed to be used in any contexts.
* Unsafe pointer types are not allowed to represent unsafe memory projections from owned types. Note: unsafe pointer types *are* allowed to represent memory projections from reference types. Note: the Swift compiler assumes that global and static functions do not return projections.
* Iterators are not allowed to represent unsafe memory projections from owned types. Iterators may be used safely through the `CxxIterator` and `CxxSequence` protocols which iterators and ranges automatically conform to.
* Members of an unsafe type that is explicitly imported using the `import_unsafe` swift attribute are still subject to the above rules. Each individual unsafe member must also have the `import_unsafe` swift attribute to be usable.

