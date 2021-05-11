//===--- Demangle.h - Interface to Swift symbol demangling ------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2019 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file is the public API of the demangler library.
// Tools which use the demangler library (like lldb) must include this - and
// only this - header file.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_DEMANGLING_DEMANGLE_H
#define SWIFT_DEMANGLING_DEMANGLE_H

#include <memory>
#include <string>
#include <cassert>
#include <cstdint>
#include "llvm/ADT/StringRef.h"
#include "swift/Runtime/Config.h"
#include "swift/Demangling/NamespaceMacros.h"

namespace llvm {
  class raw_ostream;
}

namespace swift {
namespace Demangle {
SWIFT_BEGIN_INLINE_NAMESPACE

enum class SymbolicReferenceKind : uint8_t;

/// A simple default implementation that assigns letters to type parameters in
/// alphabetic order.
std::string genericParameterName(uint64_t depth, uint64_t index);

/// Display style options for the demangler.
struct DemangleOptions {
  bool SynthesizeSugarOnTypes = false;
  bool QualifyEntities = true;
  bool DisplayExtensionContexts = true;
  bool DisplayUnmangledSuffix = true;
  bool DisplayModuleNames = true;
  bool DisplayGenericSpecializations = true;
  bool DisplayProtocolConformances = true;
  bool DisplayWhereClauses = true;
  bool DisplayEntityTypes = true;
  bool DisplayLocalNameContexts = true;
  bool ShortenPartialApply = false;
  bool ShortenThunk = false;
  bool ShortenValueWitness = false;
  bool ShortenArchetype = false;
  bool ShowPrivateDiscriminators = true;
  bool ShowFunctionArgumentTypes = true;
  bool DisplayDebuggerGeneratedModule = true;
  bool DisplayStdlibModule = true;
  bool DisplayObjCModule = true;
  bool PrintForTypeName = false;
  bool ShowAsyncResumePartial = true;

  /// If this is nonempty, entities in this module name will not be qualified.
  llvm::StringRef HidingCurrentModule;
  /// A function to render generic parameter names.
  std::function<std::string(uint64_t, uint64_t)> GenericParameterName =
      genericParameterName;

  DemangleOptions() {}

  static DemangleOptions SimplifiedUIDemangleOptions() {
    auto Opt = DemangleOptions();
    Opt.SynthesizeSugarOnTypes = true;
    Opt.QualifyEntities = true;
    Opt.DisplayExtensionContexts = false;
    Opt.DisplayUnmangledSuffix = false;
    Opt.DisplayModuleNames = false;
    Opt.DisplayGenericSpecializations = false;
    Opt.DisplayProtocolConformances = false;
    Opt.DisplayWhereClauses = false;
    Opt.DisplayEntityTypes = false;
    Opt.ShortenPartialApply = true;
    Opt.ShortenThunk = true;
    Opt.ShortenValueWitness = true;
    Opt.ShortenArchetype = true;
    Opt.ShowPrivateDiscriminators = false;
    Opt.ShowFunctionArgumentTypes = false;
    return Opt;
  };
};

class Node;
using NodePointer = Node *;

enum class FunctionSigSpecializationParamKind : unsigned {
  // Option Flags use bits 0-5. This give us 6 bits implying 64 entries to
  // work with.
  ConstantPropFunction = 0,
  ConstantPropGlobal = 1,
  ConstantPropInteger = 2,
  ConstantPropFloat = 3,
  ConstantPropString = 4,
  ClosureProp = 5,
  BoxToValue = 6,
  BoxToStack = 7,

  // Option Set Flags use bits 6-31. This gives us 26 bits to use for option
  // flags.
  Dead = 1 << 6,
  OwnedToGuaranteed = 1 << 7,
  SROA = 1 << 8,
  GuaranteedToOwned = 1 << 9,
  ExistentialToGeneric = 1 << 10,
};

enum class AutoDiffFunctionKind : char {
  JVP = 'f',
  VJP = 'r',
  Differential = 'd',
  Pullback = 'p',
};

enum class MangledDifferentiabilityKind : char {
  NonDifferentiable = 0,
  Forward = 'f',
  Reverse = 'r',
  Normal = 'd',
  Linear = 'l',
};

/// The pass that caused the specialization to occur. We use this to make sure
/// that two passes that generate similar changes do not yield the same
/// mangling. This currently cannot happen, so this is just a safety measure
/// that creates separate name spaces.
enum class SpecializationPass : uint8_t {
  AllocBoxToStack,
  ClosureSpecializer,
  CapturePromotion,
  CapturePropagation,
  FunctionSignatureOpts,
  GenericSpecializer,
};

static inline char encodeSpecializationPass(SpecializationPass Pass) {
  return char(uint8_t(Pass)) + '0';
}

enum class ValueWitnessKind {
#define VALUE_WITNESS(MANGLING, NAME) \
  NAME,
#include "swift/Demangling/ValueWitnessMangling.def"
};

enum class Directness {
  Direct, Indirect
};

class NodeFactory;
class Context;

class Node {
public:
  enum class Kind : uint16_t {
#define NODE(ID) ID,
#include "swift/Demangling/DemangleNodes.def"
  };

  using IndexType = uint64_t;

  friend class NodeFactory;
  
private:

  struct NodeVector {
    NodePointer *Nodes;
    uint32_t Number = 0;
    uint32_t Capacity = 0;
  };

  union {
    llvm::StringRef Text;
    IndexType Index;
    NodePointer InlineChildren[2];
    NodeVector Children;
  };


  Kind NodeKind;

  enum class PayloadKind : uint8_t {
    None, Text, Index, OneChild, TwoChildren, ManyChildren
  };
  PayloadKind NodePayloadKind;

  Node(Kind k)
      : NodeKind(k), NodePayloadKind(PayloadKind::None) {
  }
  Node(Kind k, llvm::StringRef t)
      : NodeKind(k), NodePayloadKind(PayloadKind::Text) {
    Text = t;
  }
  Node(Kind k, IndexType index)
      : NodeKind(k), NodePayloadKind(PayloadKind::Index) {
    Index = index;
  }
  Node(const Node &) = delete;
  Node &operator=(const Node &) = delete;

public:
  Kind getKind() const { return NodeKind; }

  bool hasText() const { return NodePayloadKind == PayloadKind::Text; }
  llvm::StringRef getText() const {
    assert(hasText());
    return Text;
  }

  bool hasIndex() const { return NodePayloadKind == PayloadKind::Index; }
  uint64_t getIndex() const {
    assert(hasIndex());
    return Index;
  }

  using iterator = const NodePointer *;

  size_t getNumChildren() const;

  bool hasChildren() const { return getNumChildren() != 0; }

  iterator begin() const;

  iterator end() const;

  NodePointer getFirstChild() const {
    return getChild(0);
  }
  NodePointer getLastChild() const {
    return getChild(getNumChildren() - 1);
  }
  NodePointer getChild(size_t index) const {
    assert(getNumChildren() > index);
    return begin()[index];
  }

  // Only to be used by the demangler parsers.
  void addChild(NodePointer Child, NodeFactory &Factory);
  // Only to be used by the demangler parsers.
  void removeChildAt(unsigned Pos);

  // Reverses the order of children.
  void reverseChildren(size_t StartingAt = 0);

  /// Prints the whole node tree in readable form to stderr.
  ///
  /// Useful to be called from the debugger.
  void dump();
};

/// Returns the length of the swift mangling prefix of the \p SymbolName.
///
/// Returns 0 if \p SymbolName is not a mangled swift (>= swift 4.x) name.
int getManglingPrefixLength(llvm::StringRef mangledName);

/// Returns true if \p SymbolName is a mangled swift name.
///
/// This does not include the old (<= swift 3.x) mangling prefix "_T".
inline bool isMangledName(llvm::StringRef mangledName) {
  return getManglingPrefixLength(mangledName) != 0;
}

/// Returns true if the mangledName starts with the swift mangling prefix.
///
/// This includes the old (<= swift 3.x) mangling prefix "_T".
bool isSwiftSymbol(llvm::StringRef mangledName);

/// Returns true if the mangledName starts with the swift mangling prefix.
///
/// This includes the old (<= swift 3.x) mangling prefix "_T".
bool isSwiftSymbol(const char *mangledName);

/// Drops the Swift mangling prefix from the given mangled name, if there is
/// one.
///
/// This does not include the old (<= swift 3.x) mangling prefix "_T".
llvm::StringRef dropSwiftManglingPrefix(llvm::StringRef mangledName);

/// Returns true if the mangled name is an alias type name.
///
/// \param mangledName A null-terminated string containing a mangled name.
bool isAlias(llvm::StringRef mangledName);

/// Returns true if the mangled name is a class type name.
///
/// \param mangledName A null-terminated string containing a mangled name.
bool isClass(llvm::StringRef mangledName);

/// Returns true if the mangled name is an enum type name.
///
/// \param mangledName A null-terminated string containing a mangled name.
bool isEnum(llvm::StringRef mangledName);

/// Returns true if the mangled name is a protocol type name.
///
/// \param mangledName A null-terminated string containing a mangled name.
bool isProtocol(llvm::StringRef mangledName);

/// Returns true if the mangled name is a structure type name.
///
/// \param mangledName A null-terminated string containing a mangled name.
bool isStruct(llvm::StringRef mangledName);

/// Returns true if the mangled name is an Objective-C symbol.
///
/// \param mangledName A null-terminated string containing a mangled name.
bool isObjCSymbol(llvm::StringRef mangledName);

/// Returns true if the mangled name has the old scheme of function type
/// mangling where labels are part of the type.
///
/// \param mangledName A null-terminated string containing a mangled name.
bool isOldFunctionTypeMangling(llvm::StringRef mangledName);

class Demangler;

/// The demangler context.
///
/// It owns the allocated nodes which are created during demangling.
/// It is always preferable to use the demangling via this context class as it
/// ensures efficient memory management. Especially if demangling is done for
/// multiple symbols. Typical usage:
/// \code
///   Context Ctx;
///   for (...) {
///      NodePointer Root = Ctx.demangleSymbolAsNode(MangledName);
///      // Do something with Root
///      Ctx.clear(); // deallocates Root
///   }
/// \endcode
/// Declaring the context out of the loop minimizes the amount of needed memory
/// allocations.
///
class Context {
  Demangler *D;

  friend class Node;

public:
  Context();

  ~Context();

  /// Demangle the given symbol and return the parse tree.
  ///
  /// \param MangledName The mangled symbol string, which start a mangling
  /// prefix: _T, _T0, $S, _$S.
  ///
  /// \returns A parse tree for the demangled string - or a null pointer
  /// on failure.
  /// The lifetime of the returned node tree ends with the lifetime of the
  /// context or with a call of clear().
  NodePointer demangleSymbolAsNode(llvm::StringRef MangledName);

  /// Demangle the given type and return the parse tree.
  ///
  /// \param MangledName The mangled symbol string, which start a mangling
  /// prefix: _T, _T0, $S, _$S.
  ///
  /// \returns A parse tree for the demangled string - or a null pointer
  /// on failure.
  /// The lifetime of the returned node tree ends with the lifetime of the
  /// context or with a call of clear().
  NodePointer demangleTypeAsNode(llvm::StringRef MangledName);
  
  /// Demangle the given symbol and return the readable name.
  ///
  /// \param MangledName The mangled symbol string, which start a mangling
  /// prefix: _T, _T0, $S, _$S.
  ///
  /// \returns The demangled string.
  std::string demangleSymbolAsString(
      llvm::StringRef MangledName,
      const DemangleOptions &Options = DemangleOptions());

  /// Demangle the given type and return the readable name.
  ///
  /// \param MangledName The mangled type string, which does _not_ start with
  /// a mangling prefix.
  ///
  /// \returns The demangled string.
  std::string
  demangleTypeAsString(llvm::StringRef MangledName,
                       const DemangleOptions &Options = DemangleOptions());

  /// Returns true if the mangledName refers to a thunk function.
  ///
  /// Thunk functions are either (ObjC) partial apply forwarder, swift-as-ObjC
  /// or ObjC-as-swift thunks or allocating init functions.
  bool isThunkSymbol(llvm::StringRef MangledName);

  /// Returns the mangled name of the target of a thunk.
  ///
  /// \returns Returns the remaining name after removing the thunk mangling
  /// characters from \p MangledName. If \p MangledName is not a thunk symbol
  /// or the thunk target cannot be derived from the mangling, an empty string
  /// is returned.
  std::string getThunkTarget(llvm::StringRef MangledName);

  /// Returns true if the \p mangledName refers to a function which conforms to
  /// the Swift calling convention.
  ///
  /// The return value is unspecified if the \p MangledName does not refer to a
  /// function symbol.
  bool hasSwiftCallingConvention(llvm::StringRef MangledName);

  /// Demangle the given symbol and return the module name of the symbol.
  ///
  /// \param mangledName The mangled symbol string, which start a mangling
  /// prefix: _T, _T0, $S, _$S.
  ///
  /// \returns The module name.
  std::string getModuleName(llvm::StringRef mangledName);

  /// Deallocates all nodes.
  ///
  /// The memory which is used for nodes is not freed but recycled for the next
  /// demangling operation.
  void clear();
};

/// Standalone utility function to demangle the given symbol as string.
///
/// If performance is an issue when demangling multiple symbols,
/// Context::demangleSymbolAsString should be used instead.
/// \param mangledName The mangled name string pointer.
/// \param mangledNameLength The length of the mangledName string.
/// \returns The demangled string.
std::string
demangleSymbolAsString(const char *mangledName, size_t mangledNameLength,
                       const DemangleOptions &options = DemangleOptions());

/// Standalone utility function to demangle the given symbol as string.
///
/// If performance is an issue when demangling multiple symbols,
/// Context::demangleSymbolAsString should be used instead.
/// \param mangledName The mangled name string.
/// \returns The demangled string.
inline std::string
demangleSymbolAsString(const std::string &mangledName,
                       const DemangleOptions &options = DemangleOptions()) {
  return demangleSymbolAsString(mangledName.data(), mangledName.size(),
                                options);
}
  
/// Standalone utility function to demangle the given symbol as string.
///
/// If performance is an issue when demangling multiple symbols,
/// Context::demangleSymbolAsString should be used instead.
/// \param MangledName The mangled name string.
/// \returns The demangled string.
inline std::string
demangleSymbolAsString(llvm::StringRef MangledName,
                       const DemangleOptions &Options = DemangleOptions()) {
  return demangleSymbolAsString(MangledName.data(),
                                MangledName.size(), Options);
}

/// Standalone utility function to demangle the given type as string.
///
/// If performance is an issue when demangling multiple symbols,
/// Context::demangleTypeAsString should be used instead.
/// \param mangledName The mangled name string pointer.
/// \param mangledNameLength The length of the mangledName string.
/// \returns The demangled string.
std::string
demangleTypeAsString(const char *mangledName, size_t mangledNameLength,
                     const DemangleOptions &options = DemangleOptions());

/// Standalone utility function to demangle the given type as string.
///
/// If performance is an issue when demangling multiple symbols,
/// Context::demangleTypeAsString should be used instead.
/// \param mangledName The mangled name string.
/// \returns The demangled string.
inline std::string
demangleTypeAsString(const std::string &mangledName,
                     const DemangleOptions &options = DemangleOptions()) {
  return demangleTypeAsString(mangledName.data(), mangledName.size(), options);
}

/// Standalone utility function to demangle the given type as string.
///
/// If performance is an issue when demangling multiple symbols,
/// Context::demangleTypeAsString should be used instead.
/// \param MangledName The mangled name string.
/// \returns The demangled string.
inline std::string
demangleTypeAsString(llvm::StringRef MangledName,
                     const DemangleOptions &Options = DemangleOptions()) {
  return demangleTypeAsString(MangledName.data(),
                              MangledName.size(), Options);
}
  

enum class OperatorKind {
  NotOperator,
  Prefix,
  Postfix,
  Infix,
};

/// Remangle a demangled parse tree.
std::string mangleNode(NodePointer root);

using SymbolicResolver =
  llvm::function_ref<Demangle::NodePointer (SymbolicReferenceKind,
                                            const void *)>;

/// Remangle a demangled parse tree, using a callback to resolve
/// symbolic references.
std::string mangleNode(NodePointer root, SymbolicResolver resolver);

/// Remangle a demangled parse tree, using a callback to resolve
/// symbolic references.
///
/// The returned string is owned by \p Factory. This means \p Factory must stay
/// alive as long as the returned string is used.
llvm::StringRef mangleNode(NodePointer root, SymbolicResolver resolver,
                           NodeFactory &Factory);

/// Remangle in the old mangling scheme.
///
/// This is only used for objc-runtime names.
std::string mangleNodeOld(NodePointer root);

/// Remangle in the old mangling scheme.
///
/// This is only used for objc-runtime names.
/// The returned string is owned by \p Factory. This means \p Factory must stay
/// alive as long as the returned string is used.
llvm::StringRef mangleNodeOld(NodePointer node, NodeFactory &Factory);

/// Remangle in the old mangling scheme and embed the name in "_Tt<name>_".
///
/// The returned string is null terminated and owned by \p Factory. This means
/// \p Factory must stay alive as long as the returned string is used.
const char *mangleNodeAsObjcCString(NodePointer node, NodeFactory &Factory);

/// Transform the node structure to a string.
///
/// Typical usage:
/// \code
///   std::string aDemangledName =
/// swift::Demangler::nodeToString(aNode)
/// \endcode
///
/// \param Root A pointer to a parse tree generated by the demangler.
/// \param Options An object encapsulating options to use to perform this demangling.
///
/// \returns A string representing the demangled name.
///
std::string nodeToString(NodePointer Root,
                         const DemangleOptions &Options = DemangleOptions());

/// A class for printing to a std::string.
class DemanglerPrinter {
public:
  DemanglerPrinter() = default;

  DemanglerPrinter &operator<<(llvm::StringRef Value) & {
    Stream.append(Value.data(), Value.size());
    return *this;
  }
  
  DemanglerPrinter &operator<<(char c) & {
    Stream.push_back(c);
    return *this;
  }
  DemanglerPrinter &operator<<(unsigned long long n) &;
  DemanglerPrinter &operator<<(long long n) &;
  DemanglerPrinter &operator<<(unsigned long n) & {
    return *this << (unsigned long long)n;
  }
  DemanglerPrinter &operator<<(long n) & {
    return *this << (long long)n;
  }
  DemanglerPrinter &operator<<(unsigned n) & {
    return *this << (unsigned long long)n;
  }
  DemanglerPrinter &operator<<(int n) & {
    return *this << (long long)n;
  }
  
  template<typename T>
  DemanglerPrinter &&operator<<(T &&x) && {
    return std::move(*this << std::forward<T>(x));
  }
  
  DemanglerPrinter &writeHex(unsigned long long n) &;
 
  std::string &&str() && { return std::move(Stream); }

  llvm::StringRef getStringRef() const { return Stream; }

  /// Shrinks the buffer.
  void resetSize(size_t toPos) {
    assert(toPos <= Stream.size());
    Stream.resize(toPos);
  }
private:
  std::string Stream;
};

/// Returns a the node kind \p k as string.
const char *getNodeKindString(swift::Demangle::Node::Kind k);

/// Prints the whole node tree \p Root in readable form into a std::string.
///
/// Useful for debugging.
std::string getNodeTreeAsString(NodePointer Root);

bool nodeConsumesGenericArgs(Node *node);

bool isSpecialized(Node *node);

NodePointer getUnspecialized(Node *node, NodeFactory &Factory);

/// Returns true if the node \p kind refers to a context node, e.g. a nominal
/// type or a function.
bool isContext(Node::Kind kind);

/// Returns true if the node \p kind refers to a node which is placed before a
/// function node, e.g. a specialization attribute.
bool isFunctionAttr(Node::Kind kind);

/// Form a StringRef around the mangled name starting at base, if the name may
/// contain symbolic references.
llvm::StringRef makeSymbolicMangledNameStringRef(const char *base);

SWIFT_END_INLINE_NAMESPACE
} // end namespace Demangle
} // end namespace swift

// NB: This function is not used directly in the Swift codebase, but is
// exported for Xcode support and is used by the sanitizers. Please coordinate
// before changing.
//
/// Demangles a Swift symbol name.
///
/// \param mangledName is the symbol name that needs to be demangled.
/// \param mangledNameLength is the length of the string that should be
/// demangled.
/// \param outputBuffer is the user provided buffer where the demangled name
/// will be placed. If nullptr, a new buffer will be malloced. In that case,
/// the user of this API is responsible for freeing the returned buffer.
/// \param outputBufferSize is the size of the output buffer. If the demangled
/// name does not fit into the outputBuffer, the output will be truncated and
/// the size will be updated, indicating how large the buffer should be.
/// \param flags can be used to select the demangling style. TODO: We should
//// define what these will be.
/// \returns the demangled name. Returns nullptr if the input String is not a
/// Swift mangled name.
SWIFT_CC(swift)
SWIFT_RUNTIME_EXPORT
char *swift_demangle(const char *mangledName,
                     size_t mangledNameLength,
                     char *outputBuffer,
                     size_t *outputBufferSize,
                     uint32_t flags);

#endif // SWIFT_DEMANGLING_DEMANGLE_H
