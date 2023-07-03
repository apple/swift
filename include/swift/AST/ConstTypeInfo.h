//===--- ConstTypeInfo.h - Const Nominal Type Info Structure ----*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2022 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_AST_CONST_TYPE_INFO_H
#define SWIFT_AST_CONST_TYPE_INFO_H

#include "swift/AST/Attr.h"
#include "swift/AST/Type.h"
#include <memory>
#include <string>
#include <vector>

namespace swift {
class NominalTypeDecl;
class VarDecl;
class Type;

/// Representation of a compile-time-known value, for example
/// in a type property initializer expression
class CompileTimeValue {
public:
  enum ValueKind {
    RawLiteral,
    InitCall,
    Builder,
    Dictionary,
    Array,
    Tuple,
    Enum,
    Type,
    Runtime
  };

  ValueKind getKind() const { return Kind; }

protected:
  CompileTimeValue(ValueKind ValueKind) : Kind(ValueKind) {}

private:
  ValueKind Kind;
};

/// A string representation of a raw literal value,
/// for example an integer or string or float literal.
class RawLiteralValue : public CompileTimeValue {
public:
  RawLiteralValue(std::string Value)
  : CompileTimeValue(ValueKind::RawLiteral), Value(Value) {}

  std::string getValue() const { return Value; }

  static bool classof(const CompileTimeValue *T) {
    return T->getKind() == ValueKind::RawLiteral;
  }

private:
  std::string Value;
};

struct FunctionParameter {
  std::string Label;
  swift::Type Type;
  std::shared_ptr<CompileTimeValue> Value;
};

/// A representation of a call to a type's initializer
/// with a collection of (potentially compile-time-known) parameters
class InitCallValue : public CompileTimeValue {
public:
  InitCallValue(swift::Type Type, std::vector<FunctionParameter> Parameters)
      : CompileTimeValue(ValueKind::InitCall), Type(Type),
        Parameters(Parameters) {}

  static bool classof(const CompileTimeValue *T) {
    return T->getKind() == ValueKind::InitCall;
  }

  swift::Type getType() const { return Type; }
  std::vector<FunctionParameter> getParameters() const { return Parameters; }

private:
  swift::Type Type;
  std::vector<FunctionParameter> Parameters;
};

/// A representation of a Builder pattern initialization expression
class BuilderValue : public CompileTimeValue {
public:
  BuilderValue() : CompileTimeValue(ValueKind::Builder) {}

  static bool classof(const CompileTimeValue *T) {
    return T->getKind() == ValueKind::Builder;
  }

private:
  std::vector<CompileTimeValue> Members;
};

struct TupleElement {
  llvm::Optional<std::string> Label;
  swift::Type Type;
  std::shared_ptr<CompileTimeValue> Value;
};

/// A representation of a tuple and each tuple-element
class TupleValue : public CompileTimeValue {
public:
  TupleValue(std::vector<TupleElement> Elements)
      : CompileTimeValue(ValueKind::Tuple), Elements(Elements) {}

  static bool classof(const CompileTimeValue *T) {
    return T->getKind() == ValueKind::Tuple;
  }

  std::vector<TupleElement> getElements() const { return Elements; }

private:
  std::vector<TupleElement> Elements;
};

/// An array literal value representation
class ArrayValue : public CompileTimeValue {
public:
  ArrayValue(std::vector<std::shared_ptr<CompileTimeValue>> Elements)
      : CompileTimeValue(ValueKind::Array), Elements(Elements) {}

  static bool classof(const CompileTimeValue *T) {
    return T->getKind() == ValueKind::Array;
  }
  std::vector<std::shared_ptr<CompileTimeValue>> getElements() const {
    return Elements;
  }

private:
  std::vector<std::shared_ptr<CompileTimeValue>> Elements;
};

/// A dictionary literal value representation
class DictionaryValue : public CompileTimeValue {
public:
  DictionaryValue(std::vector<std::shared_ptr<TupleValue>> elements)
      : CompileTimeValue(ValueKind::Dictionary), Elements(elements) {}

  static bool classof(const CompileTimeValue *T) {
    return T->getKind() == ValueKind::Dictionary;
  }

  std::vector<std::shared_ptr<TupleValue>> getElements() const {
    return Elements;
  }

private:
  std::vector<std::shared_ptr<TupleValue>> Elements;
};

/// An enum value representation
class EnumValue : public CompileTimeValue {
public:
  EnumValue(std::string Identifier,
            llvm::Optional<std::vector<FunctionParameter>> Parameters)
      : CompileTimeValue(ValueKind::Enum), Identifier(Identifier),
        Parameters(Parameters) {}

  std::string getIdentifier() const { return Identifier; }
  llvm::Optional<std::vector<FunctionParameter>> getParameters() const {
    return Parameters;
  }

  static bool classof(const CompileTimeValue *T) {
    return T->getKind() == ValueKind::Enum;
  }

private:
  std::string Identifier;
  llvm::Optional<std::vector<FunctionParameter>> Parameters;
};

/// An type value representation
class TypeValue : public CompileTimeValue {
public:
  TypeValue(swift::Type Type) : CompileTimeValue(ValueKind::Type), Type(Type) {}

  swift::Type getType() const { return Type; }

  static bool classof(const CompileTimeValue *T) {
    return T->getKind() == ValueKind::Type;
  }

private:
  swift::Type Type;
};

/// A representation of an arbitrary value that does not fall under
/// any of the above categories.
class RuntimeValue : public CompileTimeValue {
public:
  RuntimeValue() : CompileTimeValue(ValueKind::Runtime) {}

  static bool classof(const CompileTimeValue *T) {
    return T->getKind() == ValueKind::Runtime;
  }
};

struct CustomAttrValue {
  const swift::CustomAttr *Attr;
  std::vector<FunctionParameter> Parameters;
};

/// A representation of a single associated value for an enumeration case.
struct EnumElementParameterValue {
  llvm::Optional<std::string> Label;
  swift::Type Type;
};

/// A representation of a single enumeration case.
struct EnumElementDeclValue {
  std::string Name;
  llvm::Optional<std::string> RawValue;
  llvm::Optional<std::vector<EnumElementParameterValue>> Parameters;
};

using AttrValueVector = llvm::SmallVector<CustomAttrValue, 2>;
struct ConstValueTypePropertyInfo {
  swift::VarDecl *VarDecl;
  std::shared_ptr<CompileTimeValue> Value;
  llvm::Optional<AttrValueVector> PropertyWrappers;
  llvm::Optional<AttrValueVector> RuntimeMetadataAttributes;

  ConstValueTypePropertyInfo(
      swift::VarDecl *VarDecl, std::shared_ptr<CompileTimeValue> Value,
      llvm::Optional<AttrValueVector> PropertyWrappers,
      llvm::Optional<AttrValueVector> RuntimeMetadataAttributes)
      : VarDecl(VarDecl), Value(Value), PropertyWrappers(PropertyWrappers),
        RuntimeMetadataAttributes(RuntimeMetadataAttributes) {}

  ConstValueTypePropertyInfo(swift::VarDecl *VarDecl,
                             std::shared_ptr<CompileTimeValue> Value)
      : VarDecl(VarDecl), Value(Value),
        PropertyWrappers(llvm::Optional<AttrValueVector>()),
        RuntimeMetadataAttributes(llvm::Optional<AttrValueVector>()) {}
};

struct ConstValueTypeInfo {
  swift::NominalTypeDecl *TypeDecl;
  std::vector<ConstValueTypePropertyInfo> Properties;
  llvm::Optional<std::vector<EnumElementDeclValue>> EnumElements;
};
} // namespace swift
#endif
