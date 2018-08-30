//===--- TFUtilities.h - TensorFlow lowering utilities ----------*- C++ -*-===//
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
// This defines the shared code that implements the various TensorFlow related
// lowerings and other transformations.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SILOPTIMIZER_TENSORFLOW_H
#define SWIFT_SILOPTIMIZER_TENSORFLOW_H

#include "TFDeviceSupport.h"
#include "swift/AST/TensorFlow.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILFunction.h"
#ifdef SWIFT_ENABLE_TENSORFLOW
#include "tensorflow/c/c_api.h"
#endif

namespace swift {
namespace tf {

// TODO: reformat the code below to remove indentation.

/// If the -tf-dump-intermediates flag has been passed, return a pointer to
/// the stream that we should print debug dump information to.  Otherwise,
/// return null.  This is used for integration unit tests and debugging.
llvm::raw_ostream *getTFDumpIntermediateStream();

/// If the specified decl has a single stored field, return it.  Otherwise
/// return null.
VarDecl *getFieldIfContainsSingleField(NominalTypeDecl *decl);

/// Return true if the specified type is the well-known TensorHandle<T> type.
bool isTensorHandle(SILType ty);
  
/// Return true if the specified type is the well-known opaque handle type such
/// as VariantHandle and ResourceHandle.
bool isOpaqueHandle(SILType ty);

/// Determine whether the specified type is one of our well-known types, and
/// if so, which one it is.
TFValueKind classifyTensorFlowValue(SILType ty);

/// Return true if the specified type is TensorHandle<T>, ResourceHandle, or
/// VariantHandle.
bool isTensorFlowValue(SILType ty);

/// This function maps a Swift type (either a language type like Float or an
/// LLVM Builtin type like Builtin.f32) into the TensorFlow TF_DataType value.
unsigned convertSwiftTypeToTF(Type ty);

/// `ty` must be a valid TensorFlow element type "T", like Builtin.Int32. Turn
/// it into a TensorHandle<T> type.
SILType convertElementTypeToTensorValueType(Type ty, const ASTContext &ctx);

/// If the specified type is a TensorFlow value type, return it.  Otherwise, it
/// must be a primitive type T.  In that case, wrap it to form TensorHandle<T>.
SILType convertElementTypeToTensorValueType(SILType ty);

/// Return true if the specified type is a valid tensor element type.  For
/// example, int128 and pointers are not.
///
/// TODO: This should eventually consider information about the target
/// deployment.
inline bool isValidTensorFlowElementType(Type ty) {
  return convertSwiftTypeToTF(ty) != 0;
}

/// Looks up a function by `name` in the context of `typeDecl`, `proto` and
/// `module`, and returns that function.
SILFunction *findSILFunctionForRequiredProtocolMember(NominalTypeDecl *typeDecl,
                                                      ProtocolDecl *proto,
                                                      DeclName name,
                                                      ModuleDecl *module,
                                                      SILModule &silModule);

/// Given an element type like `Float` and a generic signature with a single
/// type parameter, returns a substitution map suitable for calling a builtin
/// or function with such a substitution.
SubstitutionMap getSingleSubstitutionMapForElementTypeAndSignature(
    Type ty, GenericSignature *genericSig);

/// Given an element type like `Float`, returns a substitution map suitable
/// for calling a builtin or function with this single-entry substitution.
SubstitutionMap getSingleSubstitutionMapForElementType(Type ty,
                                                       ASTContext &ctx);

/// Holds information about a TensorFlow operation as represented in SIL
/// as Builtin instructions.
struct SILTensorOpInfo {
  /// The instruction being analyzed.
  BuiltinInst *inst;

  /// This is the name for the entire builtin that we'll partition out.
  StringRef builtinName;

  /// This is the TensorFlow name for the op.
  StringRef opName;

  /// One of these records exists for every operand that the BuiltinInst has,
  /// classifying the operand into a couple of buckets.  The most coarse grain
  /// classification is "input" vs "attribute": the inputs come first,
  /// followed by the attributes.  However, we need to be able to model the
  /// fact that some input arguments are aggregated together into a single
  /// input that is an array of tensors.  An integer attribute may be either
  /// a Tensor value or an integer-encoded DType, etc.
  enum class OperandClass {
    /// Indicates one of the following:
    /// 1) A normal tensor input: the value is a TensorHandle.
    /// 2) An normal attribute (without modifier).
    /// 3) A tensor or shape attribute (need a modifier for proper lowering).
    /// 4) An array attribute (needed for parsing tfop, and dropped before graph
    ///    lowering).
    Input,

    /// No modifier.
    Normal,

    /// Indicates that the array or scalar should be turned into a TF_Tensor.
    Tensor,

    /// Indicates that the array of integers should be interpreted as a shape.
    Shape,

    /// Indicates the metatype of a TensorFlow value type or an aggregate of
    /// TensorFlow value types should be turned into a list of unknown shapes.
    UnknownShapeList,

    /// Indicates that the operand should be interpreted as an array. When
    /// applied to the metatype of a TensorFlow value type or an aggregate of
    /// TensorFlow value types, it will be flattened into an array of dtypes of
    /// each TensorFlow value type as a Normal operand.
    Array,

    /// An operand specifying the address where an indirect output should be
    /// stored.  This occurs when the tfop exists in a context where its output
    /// is address-only.  Deabstraction eliminates Out operands before forming
    /// graph_ops, by rewriting the tfop to return the value directly.  This
    /// rewriting is possible because tfop outputs must always be loadable in
    /// deabstraction scopes.
    Out,
  };

  /// Return the string suffix for the specified attribute modifier.
  static const char *getOperandClassSuffix(OperandClass opClass);

  /// Return the operand class of the specified string form like "tensor"
  static llvm::Optional<OperandClass> getOperandClass(StringRef suffix);

  /// These are the names of any attribute operands at the end of the list.
  SmallVector<std::pair<StringRef, OperandClass>, 4> operandClasses;

  /// Return true if the specified operand is an input (not an attribute).
  bool isInput(unsigned operandNumber) const {
    return operandClasses[operandNumber].second == OperandClass::Input;
  }

  /// Returns the full name that this builtin would have if its operands
  /// changed to the passed-in values.
  std::string getBuiltinNameWithNewOperands(
      const SmallVectorImpl<std::pair<StringRef, OperandClass>>
        &newOperandClasses) const {
    std::string name = "__tfop_" + opName.str();
    for (const auto &newOperandClass : newOperandClasses) {
      name += ",";
      name += newOperandClass.first;
      name += getOperandClassSuffix(newOperandClass.second);
    }
    return name;
  }

  /// Analyze the specified SIL instruction and return a SILTensorOpInfo
  /// result if the instruction is a valid tensor operation.  This is the
  /// way that SILTensorOpInfo's are created.
  static Optional<SILTensorOpInfo> decode(SILInstruction *inst);

private:
  SILTensorOpInfo(BuiltinInst *inst) : inst(inst) {}
  bool decodeBuiltin();
};

/// Holds information about a TensorFlow operation as represented in SIL
/// as GraphOperationInst.
struct GraphOperationInfo {
  /// The instruction being analyzed.
  GraphOperationInst *inst;

  explicit GraphOperationInfo(GraphOperationInst *inst) : inst(inst) {}

  /// Return the device attribute associated with `inst`, which is required to
  /// exist.
  StringRef getDeviceString() const;

  /// Return the device type for this instruction.
  DeviceType getDeviceType() const {
    return getOpDeviceType(getDeviceString());
  }

  enum InputMarker {
    /// Scalar input, used by tfc.scalarToTensor only.
    IM_Scalar,
    /// Normal tensor, variant or resource input.
    IM_Normal,
    /// Marker for the start of an input list, has no corresponding operand.
    IM_InputList,
    /// Element of an input list.
    IM_InputListElt,
  };

  /// Return a comma and letter identifier whose letter corresponds to the
  /// specified InputMarker.
  static const char *getInputMarker(InputMarker kind) {
    switch (kind) {
    case IM_Scalar:
      return ",s";
    case IM_Normal:
      return ",i";
    case IM_InputList:
      return ",L";
    case IM_InputListElt:
      return ",e";
    }
  }

  /// Decode the name of a graph_op into its TensorFlow op name and a list of
  /// information about the operands.
  StringRef decodeName(SmallVectorImpl<InputMarker> &inputInfo);

  /// Given an attribute name like foo$tensor, decode the name and the class.
  /// If there is no modifier specified, this defaults to
  /// OperandClass::Normal.
  static std::pair<StringRef, SILTensorOpInfo::OperandClass>
  decodeAttributeName(Identifier name);

  /// Get an int-typed attribute at `attrIdx`, which must have `attrName`.
  int64_t getIntAttr(unsigned attrIdx, StringRef attrName) const;

  /// Get a string-typed attribute at `attrIdx`, which must have `attrName`.
  std::string getStringAttr(unsigned attrIdx, StringRef attrName) const;

  void assertWithDump(bool cond, const char *assertMsg) const;
};

/// `inst` must have a single result, and return that result value.
static inline SILValue getSingleValueResult(GraphOperationInst *inst) {
  assert(inst->getNumResults() == 1);
  return inst->getResults()[0];
}

//===--------------------------------------------------------------------===//
// Source location helpers
//===--------------------------------------------------------------------===//

/// The SIL location for operations we process are usually deep in the bowels
/// of the tensor library code, which are all implementation details to the
/// user.  As such, walk the inlining location of the specified node to return
/// the first location *outside* of the tensor implementation goop.
SILDebugLocation skipInternalLocations(SILDebugLocation loc);

/// Skip over all the internal implementation details to get the source
///  location in user code.
inline SILLocation getUserSourceLocation(SILDebugLocation loc) {
  return skipInternalLocations(loc).getLocation();
}

/// Get the user's source location for the specified value.  If it is an
/// instruction, we can apply various heuristics to improve the precision of
/// the returned location information.
SILLocation getUserSourceLocation(SILValue value);
SILLocation getUserSourceLocation(SILInstruction *inst);

//===--------------------------------------------------------------------===//
// Other stuff
//===--------------------------------------------------------------------===//

/// Create a "Const" tensor operation containing the specified scalars, with
/// the specified shape and elementType (setting dtype).  The resultType is
/// the TensorHandle type to produce, and targetDevice is the device set for
/// the operation.
GraphOperationInst *createConstTensor(Type elementType, SymbolicValue scalars,
                                      SymbolicValue shape, SILType resultType,
                                      SILLocation loc, DeviceType targetDevice,
                                      SILBuilder &B);

/// Create a tf_tensor_to_i1 instruction with the given value as argument.
GraphOperationInst *createTensorToInt1Inst(SILValue value, SILBuilder &builder,
                                           SILLocation location,
                                           GraphFunctionDeviceInfo &deviceInfo);

/// This struct provides a an efficient implementation of a predicate that
/// determines whether a type is or contains a TensorHandle that will be
/// exposed after deabstraction.  This is a class instead of a simple
/// function because we memoize state to avoid rechecking types over and
/// over again.
class TensorFunctionClassifier {
  TypeContainsTensorFlowValue tctfc;

public:
  TensorFunctionClassifier() {}

  /// Return true if the specified function is the top-level context that
  /// tensor partitioning should be applied to.  This returns false (for
  /// example) for inlined functions that take and return tensors, since we
  /// know that they are either unreachable or will be inlined into any
  /// clients that use them.
  ///
  /// If the flag forceTFFunctions is true, forces partitioning of functions
  /// that operate on Tensors even if it would have been rejected otherwise.
  bool shouldBePartitioned(SILFunction *fn, bool forceTFFunctions);

  /// Return true if the specified function type has TensorFlow values in its
  /// argument or result list (and do so recursively, if `fnType` has an
  /// argument or result that is itself function-typed), even if they are
  /// abstracted by structs or tuples.
  bool containsTensorFlowValue(CanSILFunctionType fnType);

  /// Return true if the specified type contains a TensorFlow value type that
  /// will be exposed after deabstraction.
  /// If `checkHigherOrderFunctions`, also check for a function-typed `ty`, if
  /// its parameter or result contains any TensorFlow value type.
  bool containsTensorFlowValue(Type ty, bool checkHigherOrderFunctions) {
    return tctfc.containsTensorFlowValue(ty, checkHigherOrderFunctions);
  }

  /// Return true if the specified type contains a TensorFlow value type that
  /// will be exposed after deabstraction.
  /// If `checkHigherOrderFunctions`, also check for a function-typed `ty`, if
  /// its parameter or result contains any TensorFlow value type.
  bool containsTensorFlowValue(SILType ty, bool checkHigherOrderFunctions) {
    return containsTensorFlowValue(ty.getASTType(), checkHigherOrderFunctions);
  }
};

/// Represent the TF graph of a graph function named `graphFnName`, which
/// corresponds to the SIL host function `silHostFnName`. `graph` can contain
/// more functions beyond `graphFnName`, if that function calls into other
/// graph functions (e.g. if it has functional If/While ops).
struct LoweredGraphFunction {
  LoweredGraphFunction(const std::string &silHostFnName,
                       const std::string &graphFnName)
      : silHostFnName(silHostFnName), graphFnName(graphFnName) {}

  LoweredGraphFunction(LoweredGraphFunction &&) = delete;

  /// Used as the buffer to back a StringRef-typed map key value elsewhere.
  std::string silHostFnName;

  std::string graphFnName;
};

/// Each object lowers a set of accelerator functions into a single TF graph.
class TFGraphLowering {
  llvm::DenseMap<StringRef, std::unique_ptr<LoweredGraphFunction>>
      &graphFunctions;
  std::unique_ptr<TF_Graph, decltype(&TF_DeleteGraph)> graph;
  TF_Operation *metadataNodeForTPU = nullptr;

  /// This is a counter we use to give each cross-device send/receive
  /// operation a unique ID.
  int nextTensorTransferId = 0;

public:
  TFGraphLowering(
      llvm::DenseMap<StringRef, std::unique_ptr<LoweredGraphFunction>>
          &graphFunctions);

  /// Lower the accelerator-only function `fn` (which was formed by the
  /// partitioner) into a TensorFlow graph function, and add an entry to
  /// `graphFunctions`, keyed on `hostFnName`. This way another graph
  /// function foo() can call/use this function, if the corresponding SIL
  /// code of foo() calls/uses `hostFnName`.
  bool lowerTFFunction(StringRef hostFnName, SILFunction *fn,
                       const GraphFunctionDeviceInfo &deviceInfo);

  /// Similar to the function above, except it handles a
  /// non-accelerator-only function, which can be lowered to graph functions
  /// on a set of TF devices.
  ///
  /// When deviceInfo.usedDeviceTypes has N>1 devices, in addition to
  /// generating a graph function whose name is
  /// LoweredGraphFunction::graphFnName (referred to as `entryFnBaseName`),
  /// also generate another N-1 nodes named `entryFnBaseName_helper_{i}`,
  /// with i ranging from 0 to N-2. These N nodes correspond to the N
  /// per-device graph functions, and must be called by the runtime in a
  /// single SessionRun() call. Those N-1 helper functions take no input or
  /// output tensors, and are executed for their side-effects of
  /// sending/receiving tensors with the function of `entryFnBaseName`.
  bool lowerTFGraph(StringRef hostFnName, SILFunction *fn,
                    const GraphFunctionDeviceInfo &deviceInfo);

  /// Serialize `graph` into a binary protobuf into `bytes`.
  /// Return true on error, with an error diagnostic already emitted at
  /// `errorLoc`.
  bool serializeGraphProtoBuf(ASTContext &ctx, SILLocation errorLoc,
                              std::vector<char> &bytes);

  /// Return the graph for debug printing.
  TF_Graph *getGraphDebug() { return graph.get(); }

private:
  /// This is a helper function to unify the implementation of
  /// lowerTFFunction() and lowerTFGraph(). The former calls this method with
  /// `isAcceleratorOnly` set to true, and the latter false. See their doc
  /// comments on the semantics.
  ///
  ///  `graphFnNameForCaller` provides for the caller with a name to call this
  /// lowered graph function. If `isAcceleratorOnly` is true, it is the graph
  /// function name for a TF graph node to call; otherwise, it is a function
  /// name for the host runtime to call.
  bool lowerTFGraphOrFunction(StringRef hostFnName, SILFunction *fn,
                              const std::string &graphFnNameForCaller,
                              bool isAcceleratorOnly,
                              const GraphFunctionDeviceInfo &deviceInfo);
};

} // end namespace tf
} // end namespace swift
#endif
