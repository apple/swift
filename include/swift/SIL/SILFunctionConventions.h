//===- SILFunctionConventions.h - Defines SIL func. conventions -*- C++ -*-===//
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
///
/// \file
///
/// This file defines the SILModuleConventions and SILFunctionConventions
/// classes.  These interfaces are used to determine when SIL can represent
/// values of a given lowered type by value and when they must be represented by
/// address. This is influenced by a SILModule-wide "lowered address" convention,
/// which reflects whether the current SIL stage requires lowered addresses.
///
/// The primary purpose of this API is mapping the formal SIL parameter and
/// result conventions onto the SIL argument types. The "formal" conventions are
/// immutably associated with a SILFunctionType--a SIL function's type
/// information never changes. The SIL conventions determine how those formal
/// conventions will be represented in the body of SIL functions and at call
/// sites.
///
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SIL_FUNCTIONCONVENTIONS_H
#define SWIFT_SIL_FUNCTIONCONVENTIONS_H

#include "swift/AST/Types.h"
#include "swift/SIL/SILArgumentConvention.h"
#include "swift/SIL/SILType.h"
#include "llvm/Support/ErrorHandling.h"

namespace swift {

template<bool _, template<typename...> class T, typename...Args>
struct delay_template_expansion {
  using type = T<Args...>;
};

/// Transient wrapper for SILParameterInfo and SILResultInfo conventions. This
/// abstraction helps handle the transition from canonical SIL conventions to
/// lowered SIL conventions.
class SILModuleConventions {
  friend SILParameterInfo;
  friend SILResultInfo;
  friend SILFunctionConventions;

  static bool isIndirectSILParam(SILParameterInfo param,
                                 bool loweredAddresses);

  static bool isIndirectSILYield(SILYieldInfo yield,
                                 bool loweredAddresses);

  static bool isIndirectSILResult(SILResultInfo result,
                                  bool loweredAddresses);

  static SILType getSILParamInterfaceType(
                                 SILParameterInfo yield,
                                 bool loweredAddresses);

  static SILType getSILYieldInterfaceType(
                                 SILYieldInfo yield,
                                 bool loweredAddresses);

  static SILType getSILResultInterfaceType(
                                  SILResultInfo param,
                                  bool loweredAddresses);

public:
  static bool isPassedIndirectlyInSIL(SILType type, SILModule &M);

  static bool isReturnedIndirectlyInSIL(SILType type, SILModule &M);

  static SILModuleConventions getLoweredAddressConventions(SILModule &M) {
    return SILModuleConventions(M, true);
  }

private:
  SILModule *M;
  bool loweredAddresses;
  
  SILModuleConventions(SILModule &M, bool loweredAddresses)
    : M(&M), loweredAddresses(loweredAddresses)
  {}
  
public:
  SILModuleConventions(SILModule &M);

  SILFunctionConventions getFunctionConventions(CanSILFunctionType funcTy);
  
  SILModule &getModule() const { return *M; }

  bool useLoweredAddresses() const { return loweredAddresses; }

  bool isSILIndirect(SILParameterInfo param) const {
    return isIndirectSILParam(param, loweredAddresses);
  }

  bool isSILIndirect(SILYieldInfo yield) const {
    return isIndirectSILYield(yield, loweredAddresses);
  }

  bool isSILIndirect(SILResultInfo result) const {
    return isIndirectSILResult(result, loweredAddresses);
  }

  SILType getSILType(SILParameterInfo param, CanSILFunctionType funcTy,
                     TypeExpansionContext context) const {
    auto interfaceTy = getSILParamInterfaceType(param, loweredAddresses);
    // TODO: Always require a function type
    if (funcTy)
      return funcTy->substInterfaceType(*M, interfaceTy, context);
    return interfaceTy;
  }

  SILType getSILType(SILYieldInfo yield, CanSILFunctionType funcTy,
                     TypeExpansionContext context) const {
    auto interfaceTy = getSILYieldInterfaceType(yield, loweredAddresses);
    // TODO: Always require a function type
    if (funcTy)
      return funcTy->substInterfaceType(*M, interfaceTy, context);
    return interfaceTy;
  }

  SILType getSILType(SILResultInfo result, CanSILFunctionType funcTy,
                     TypeExpansionContext context) const {
    auto interfaceTy = getSILResultInterfaceType(result, loweredAddresses);
    // TODO: Always require a function type
    if (funcTy)
      return funcTy->substInterfaceType(*M, interfaceTy, context);
    return interfaceTy;
  }
};

/// Transient wrapper for SIL-level argument conventions. This abstraction helps
/// handle the transition from canonical SIL conventions to lowered SIL
/// conventions.
class SILFunctionConventions {
public:
  SILModuleConventions silConv;
  CanSILFunctionType funcTy;

  SILFunctionConventions(CanSILFunctionType funcTy, SILModule &M)
      : silConv(M), funcTy(funcTy) {}

  SILFunctionConventions(CanSILFunctionType funcTy,
                         SILModuleConventions silConv)
      : silConv(silConv), funcTy(funcTy) {}

  //===--------------------------------------------------------------------===//
  // SILModuleConventions API for convenience.
  //===--------------------------------------------------------------------===//

  bool useLoweredAddresses() const { return silConv.useLoweredAddresses(); }

  bool isSILIndirect(SILParameterInfo param) const {
    return silConv.isSILIndirect(param);
  }

  bool isSILIndirect(SILYieldInfo yield) const {
    return silConv.isSILIndirect(yield);
  }

  bool isSILIndirect(SILResultInfo result) const {
    return silConv.isSILIndirect(result);
  }

  SILType getSILType(SILParameterInfo param,
                     TypeExpansionContext context) const {
    return silConv.getSILType(param, funcTy, context);
  }

  SILType getSILType(SILYieldInfo yield, TypeExpansionContext context) const {
    return silConv.getSILType(yield, funcTy, context);
  }

  SILType getSILType(SILResultInfo result, TypeExpansionContext context) const {
    return silConv.getSILType(result, funcTy, context);
  }

  //===--------------------------------------------------------------------===//
  // SIL results.
  //===--------------------------------------------------------------------===//

  /// Get the normal result type of an apply that calls this function.
  /// This does not include indirect SIL results.
  SILType getSILResultType(TypeExpansionContext context) {
    if (silConv.loweredAddresses)
      return funcTy->getDirectFormalResultsType(silConv.getModule(), context);

    return funcTy->getAllResultsSubstType(silConv.getModule(), context);
  }

  /// Get the SIL type for the single result which may be direct or indirect.
  SILType getSingleSILResultType(TypeExpansionContext context) {
    return getSILType(funcTy->getSingleResult(), context);
  }

  /// Get the error result type.
  SILType getSILErrorType(TypeExpansionContext context) {
    return getSILType(funcTy->getErrorResult(), context);
  }

  /// Returns an array of result info.
  /// Provides convenient access to the underlying SILFunctionType.
  ArrayRef<SILResultInfo> getResults() const {
    return funcTy->getResults();
  }

  /// Get the number of SIL results passed as address-typed arguments.
  unsigned getNumIndirectSILResults() const {
    // TODO: Return packs directly in lowered-address mode
    return silConv.loweredAddresses ? funcTy->getNumIndirectFormalResults()
                                    : funcTy->getNumPackResults();
  }

  /// Are any SIL results passed as address-typed arguments?
  bool hasIndirectSILResults() const { return getNumIndirectSILResults() != 0; }

  struct IndirectSILResultFilter {
    bool loweredAddresses;
    IndirectSILResultFilter(bool loweredAddresses)
        : loweredAddresses(loweredAddresses) {}
    bool operator()(SILResultInfo result) const {
      return (loweredAddresses ? result.isFormalIndirect() : result.isPack());
    }
  };
  using IndirectSILResultIter =
      llvm::filter_iterator<const SILResultInfo *, IndirectSILResultFilter>;
  using IndirectSILResultRange = iterator_range<IndirectSILResultIter>;

  /// Return a range of indirect result information for results passed as
  /// address-typed SIL arguments.
  IndirectSILResultRange getIndirectSILResults() const {
    return llvm::make_filter_range(
        funcTy->getResults(),
        IndirectSILResultFilter(silConv.loweredAddresses));
  }

  struct SILResultTypeFunc;

  // Gratuitous template parameter is to delay instantiating `mapped_iterator`
  // on the incomplete type SILResultTypeFunc.
  template<bool _ = false>
  using IndirectSILResultTypeIter = typename delay_template_expansion<_, 
      llvm::mapped_iterator, IndirectSILResultIter, SILResultTypeFunc>::type;
  template<bool _ = false>
  using IndirectSILResultTypeRange = iterator_range<IndirectSILResultTypeIter<_>>;

  /// Return a range of SILTypes for each result passed as an address-typed SIL
  /// argument.
  template <bool _ = false>
  IndirectSILResultTypeRange<_>
  getIndirectSILResultTypes(TypeExpansionContext context) const;

  /// Get the number of SIL results directly returned by SIL value.
  unsigned getNumDirectSILResults() const {
    return silConv.loweredAddresses ? funcTy->getNumDirectFormalResults()
                                    : funcTy->getNumResults() - funcTy->getNumPackResults();
  }

  /// Like getNumDirectSILResults but @out tuples, which are not flattened in
  /// the return type, are recursively flattened in the count.
  template <bool _ = false>
  unsigned getNumExpandedDirectSILResults(TypeExpansionContext context) const;

  struct DirectSILResultFilter {
    bool loweredAddresses;
    DirectSILResultFilter(bool loweredAddresses)
        : loweredAddresses(loweredAddresses) {}
    bool operator()(SILResultInfo result) const {
      return (loweredAddresses ? !result.isFormalIndirect() : !result.isPack());
    }
  };
  using DirectSILResultIter =
      llvm::filter_iterator<const SILResultInfo *, DirectSILResultFilter>;
  using DirectSILResultRange = iterator_range<DirectSILResultIter>;

  /// Return a range of direct result information for results directly returned
  /// by SIL value.
  DirectSILResultRange getDirectSILResults() const {
    return llvm::make_filter_range(
        funcTy->getResults(), DirectSILResultFilter(silConv.loweredAddresses));
  }

  template<bool _ = false>
  using DirectSILResultTypeIter = typename delay_template_expansion<_, 
      llvm::mapped_iterator, DirectSILResultIter, SILResultTypeFunc>::type;
  template<bool _ = false>
  using DirectSILResultTypeRange = iterator_range<DirectSILResultTypeIter<_>>;

  /// Return a range of SILTypes for each result directly returned
  /// by SIL value.
  template <bool _ = false>
  DirectSILResultTypeRange<_>
  getDirectSILResultTypes(TypeExpansionContext context) const;

  //===--------------------------------------------------------------------===//
  // SIL parameters types.
  //===--------------------------------------------------------------------===//

  /// Returns the number of function parameters, not including any formally
  /// indirect results. Provides convenient access to the underlying
  /// SILFunctionType.
  unsigned getNumParameters() const { return funcTy->getNumParameters(); }

  /// Returns an array of parameter info, not including indirect
  /// results. Provides convenient access to the underlying SILFunctionType.
  ArrayRef<SILParameterInfo> getParameters() const {
    return funcTy->getParameters();
  }

  struct SILParameterTypeFunc;
  
  // Gratuitous template parameter is to delay instantiating `mapped_iterator`
  // on the incomplete type SILParameterTypeFunc.
  template<bool _ = false>
  using SILParameterTypeIter = typename
    delay_template_expansion<_, llvm::mapped_iterator,
                          const SILParameterInfo *, SILParameterTypeFunc>::type;
  
  template<bool _ = false>
  using SILParameterTypeRange = iterator_range<SILParameterTypeIter<_>>;

  /// Return a range of SILTypes for each function parameter, not including
  /// indirect results.
  template <bool _ = false>
  SILParameterTypeRange<_>
  getParameterSILTypes(TypeExpansionContext context) const;

  //===--------------------------------------------------------------------===//
  // SIL yield types.
  //===--------------------------------------------------------------------===//

  unsigned getNumYields() const { return funcTy->getNumYields(); }

  ArrayRef<SILYieldInfo> getYields() const {
    return funcTy->getYields();
  }

  template<bool _ = false>
  using SILYieldTypeIter = typename
    delay_template_expansion<_, llvm::mapped_iterator,
                              const SILYieldInfo *, SILParameterTypeFunc>::type;
  template<bool _ = false>
  using SILYieldTypeRange = iterator_range<SILYieldTypeIter<_>>;

  template<bool _ = false>
  SILYieldTypeRange<_> getYieldSILTypes(TypeExpansionContext context) const;

  SILYieldInfo getYieldInfoForOperandIndex(unsigned opIndex) const {
    return getYields()[opIndex];
  }

  //===--------------------------------------------------------------------===//
  // SILArgument API, including indirect results and parameters.
  //
  // The argument indices below relate to full applies in which the caller and
  // callee indices match. Partial apply indices are shifted on the caller
  // side. See ApplySite::getCalleeArgIndexOfFirstAppliedArg().
  //===--------------------------------------------------------------------===//

  unsigned getSILArgIndexOfFirstIndirectResult() const { return 0; }

  unsigned getSILArgIndexOfFirstParam() const {
    return getNumIndirectSILResults();
  }

  /// Returns the index of self.
  unsigned getSILArgIndexOfSelf() const {
    return getSILArgIndexOfFirstParam() + getNumParameters() - 1;
  }

  /// Get the index into formal indirect results corresponding to the given SIL
  /// indirect result argument index.
  unsigned getIndirectFormalResultIndexForSILArg(unsigned argIdx) const {
    assert(argIdx <= getNumIndirectSILResults());
    unsigned curArgIdx = 0;
    unsigned formalIdx = 0;
    for (auto formalResult : funcTy->getIndirectFormalResults()) {
      if (isSILIndirect(formalResult)) {
        if (curArgIdx == argIdx)
          return formalIdx;
        ++curArgIdx;
      }
      ++formalIdx;
    }
    llvm_unreachable("missing indirect formal result for SIL argument.");
  }

  /// Get the total number of arguments for a full apply in SIL of
  /// this function type. This is also the total number of SILArguments
  /// in the entry block.
  unsigned getNumSILArguments() const {
    return getNumIndirectSILResults() + funcTy->getNumParameters();
  }

  SILParameterInfo getParamInfoForSILArg(unsigned index) const {
    assert(index >= getNumIndirectSILResults()
           && index <= getNumSILArguments());
    return funcTy->getParameters()[index - getNumIndirectSILResults()];
  }

  /// Return the SIL argument convention of apply/entry argument at
  /// the given argument index.
  SILArgumentConvention getSILArgumentConvention(unsigned index) const;
  // See SILArgument.h.

  /// Return the SIL type of the apply/entry argument at the given index.
  SILType getSILArgumentType(unsigned index,
                             TypeExpansionContext context) const;

  /// Returns true if this function does not return to the caller.
  bool isNoReturn(TypeExpansionContext context) const;
};

struct SILFunctionConventions::SILResultTypeFunc {
  SILFunctionConventions silConv;
  TypeExpansionContext context;
  SILResultTypeFunc(const SILFunctionConventions &silConv,
                    TypeExpansionContext context)
    : silConv(silConv), context(context) {}

  SILType operator()(SILResultInfo result) const {
    return silConv.getSILType(result, context);
  }
};

template <bool _>
SILFunctionConventions::IndirectSILResultTypeRange<_>
SILFunctionConventions::getIndirectSILResultTypes(
    TypeExpansionContext context) const {
  return llvm::map_range(getIndirectSILResults(),
                         SILResultTypeFunc(*this, context));
}

template <bool _>
SILFunctionConventions::DirectSILResultTypeRange<_>
SILFunctionConventions::getDirectSILResultTypes(
    TypeExpansionContext context) const {
  return llvm::map_range(getDirectSILResults(),
                         SILResultTypeFunc(*this, context));
}

template <bool _>
unsigned SILFunctionConventions::getNumExpandedDirectSILResults(
    TypeExpansionContext context) const {
  if (silConv.loweredAddresses)
    return funcTy->getNumDirectFormalResults();
  unsigned retval = 0;
  // Worklist of elements to flatten or count.
  SmallVector<SILType, 4> flattenedElements;
  // Seed the worklist with the direct results.
  for (auto result : getDirectSILResultTypes(context)) {
    flattenedElements.push_back(result);
  }
  // Pop elements from the worklist and either increment the count or, if the
  // type is a tuple, add its elements to the worklist.
  while (!flattenedElements.empty()) {
    auto ty = flattenedElements.pop_back_val();
    if (auto tupleType = ty.getASTType()->getAs<TupleType>()) {
      for (auto index :
           llvm::reverse(indices(tupleType->getElementTypes()))) {
        flattenedElements.push_back(ty.getTupleElementType(index));
      }
    } else {
      retval += 1;
    }
  }
  return retval;
}

struct SILFunctionConventions::SILParameterTypeFunc {
  SILFunctionConventions silConv;
  TypeExpansionContext context;
  SILParameterTypeFunc(const SILFunctionConventions &silConv,
                       TypeExpansionContext context)
      : silConv(silConv), context(context) {}

  SILType operator()(SILParameterInfo param) const {
    return silConv.getSILType(param, context);
  }
};

template <bool _>
SILFunctionConventions::SILParameterTypeRange<_>
SILFunctionConventions::getParameterSILTypes(
    TypeExpansionContext context) const {
  return llvm::map_range(funcTy->getParameters(),
                         SILParameterTypeFunc(*this, context));
}

template <bool _>
SILFunctionConventions::SILYieldTypeRange<_>
SILFunctionConventions::getYieldSILTypes(TypeExpansionContext context) const {
  return llvm::map_range(funcTy->getYields(),
                         SILParameterTypeFunc(*this, context));
}

inline SILType
SILFunctionConventions::getSILArgumentType(unsigned index,
                                           TypeExpansionContext context) const {
  assert(index <= getNumSILArguments());
  if (index < getNumIndirectSILResults()) {
    return *std::next(getIndirectSILResultTypes(context).begin(), index);
  }
  return getSILType(
      funcTy->getParameters()[index - getNumIndirectSILResults()], context);
}

inline bool
SILFunctionConventions::isNoReturn(TypeExpansionContext context) const {
  return funcTy->isNoReturnFunction(silConv.getModule(), context);
}

inline SILFunctionConventions
SILModuleConventions::getFunctionConventions(CanSILFunctionType funcTy) {
  return SILFunctionConventions(funcTy, *this);
}

inline bool SILModuleConventions::isIndirectSILParam(SILParameterInfo param,
                                                     bool loweredAddresses) {
  switch (param.getConvention()) {
  case ParameterConvention::Direct_Unowned:
  case ParameterConvention::Direct_Guaranteed:
  case ParameterConvention::Direct_Owned:
    return false;

  case ParameterConvention::Pack_Inout:
  case ParameterConvention::Pack_Owned:
  case ParameterConvention::Pack_Guaranteed:
    return true;

  case ParameterConvention::Indirect_In:
  case ParameterConvention::Indirect_In_Guaranteed:
    return (loweredAddresses ||
            param.getInterfaceType()->isOpenedExistentialWithError());
  case ParameterConvention::Indirect_Inout:
  case ParameterConvention::Indirect_InoutAliasable:
    return true;
  }
  llvm_unreachable("covered switch isn't covered?!");
}

inline bool SILModuleConventions::isIndirectSILYield(SILYieldInfo yield,
                                                     bool loweredAddresses) {
  return isIndirectSILParam(yield, loweredAddresses);
}

inline bool SILModuleConventions::isIndirectSILResult(SILResultInfo result,
                                                      bool loweredAddresses) {
  switch (result.getConvention()) {
  case ResultConvention::Indirect:
    return (loweredAddresses ||
            result.getInterfaceType()->isOpenedExistentialWithError());
  case ResultConvention::Pack:
    return true;
  case ResultConvention::Owned:
  case ResultConvention::Unowned:
  case ResultConvention::UnownedInnerPointer:
  case ResultConvention::Autoreleased:
    return false;
  }

  llvm_unreachable("Unhandled ResultConvention in switch.");
}

inline SILType SILModuleConventions::getSILParamInterfaceType(
                                                     SILParameterInfo param,
                                                     bool loweredAddresses) {
  return SILModuleConventions::isIndirectSILParam(param,loweredAddresses)
             ? SILType::getPrimitiveAddressType(param.getInterfaceType())
             : SILType::getPrimitiveObjectType(param.getInterfaceType());
}

inline SILType SILModuleConventions::getSILYieldInterfaceType(
                                                     SILYieldInfo yield,
                                                     bool loweredAddresses) {
  return getSILParamInterfaceType(yield, loweredAddresses);
}

inline SILType SILModuleConventions::getSILResultInterfaceType(
                                                      SILResultInfo result,
                                                      bool loweredAddresses) {
  return SILModuleConventions::isIndirectSILResult(result, loweredAddresses)
             ? SILType::getPrimitiveAddressType(result.getInterfaceType())
             : SILType::getPrimitiveObjectType(result.getInterfaceType());
}

inline SILType
SILParameterInfo::getSILStorageInterfaceType() const {
  return SILModuleConventions::getSILParamInterfaceType(*this, true);
}

inline SILType
SILResultInfo::getSILStorageInterfaceType() const {
  return SILModuleConventions::getSILResultInterfaceType(*this, true);
}

inline SILType
SILParameterInfo::getSILStorageType(SILModule &M,
                                    const SILFunctionType *funcTy,
                                    TypeExpansionContext context) const {
  return funcTy->substInterfaceType(M, getSILStorageInterfaceType(),
                                    context);
}

inline SILType
SILResultInfo::getSILStorageType(SILModule &M, const SILFunctionType *funcTy,
                                 TypeExpansionContext context) const {
  return funcTy->substInterfaceType(M, getSILStorageInterfaceType(),
                                    context);
}

} // end swift namespace

#endif
