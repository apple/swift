//===--- ASTSynthesis.h - Convenient Swift AST synthesis --------*- C++ -*-===//
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

#ifndef SWIFT_ASTSYNTHESIS_H
#define SWIFT_ASTSYNTHESIS_H

#include "swift/AST/Types.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/Decl.h"
#include "swift/AST/GenericParamList.h"
#include "swift/AST/ParameterList.h"

namespace swift {

struct SynthesisContext {
  ASTContext &Context;
  DeclContext *DC;
  GenericParamList *GenericParams = nullptr;

  SynthesisContext(ASTContext &ctx, DeclContext *DC)
    : Context(ctx), DC(DC) {}
};

/// Allow literal types to be passed at an arbitrary position
/// in the type-synthesis DSL.
inline Type synthesizeType(SynthesisContext &SC, Type type) {
  return type;
}

/// A synthesizer which generates a specific type.
enum SingletonTypeSynthesizer {
  _any,
  _bridgeObject,
  _error,
  _executor,
  _job,
  _nativeObject,
  _never,
  _rawPointer,
  _rawUnsafeContinuation,
  _void,
  _word,
  _serialExecutor,
};
inline Type synthesizeType(SynthesisContext &SC,
                           SingletonTypeSynthesizer kind) {
  switch (kind) {
  case _any: return SC.Context.TheAnyType;
  case _bridgeObject: return SC.Context.TheBridgeObjectType;
  case _error: return SC.Context.getExceptionType();
  case _executor: return SC.Context.TheExecutorType;
  case _job: return SC.Context.TheJobType;
  case _nativeObject: return SC.Context.TheNativeObjectType;
  case _never: return SC.Context.getNeverType();
  case _rawPointer: return SC.Context.TheRawPointerType;
  case _rawUnsafeContinuation: return SC.Context.TheRawUnsafeContinuationType;
  case _void: return SC.Context.TheEmptyTupleType;
  case _word: return BuiltinIntegerType::get(BuiltinIntegerWidth::pointer(),
                                             SC.Context);
  case _serialExecutor:
    return SC.Context.getProtocol(KnownProtocolKind::SerialExecutor)
      ->getDeclaredInterfaceType();
  }
}

/// A synthesizer which generates an integer type.
struct IntegerTypeSynthesizer {
  unsigned BitWidth;
};
constexpr inline IntegerTypeSynthesizer _int(unsigned bitWidth) {
  return {bitWidth};
}
inline Type synthesizeType(SynthesisContext &SC, IntegerTypeSynthesizer S) {
  return BuiltinIntegerType::get(S.BitWidth, SC.Context);
}

/// A synthesizer which generates a vector type.
template <class S>
struct VectorTypeSynthesizer {
  unsigned Count;
  S Sub;
};
template <class S>
constexpr VectorTypeSynthesizer<S> _vector(unsigned count, S sub) {
  return {count, sub};
}
template <class S>
Type synthesizeType(SynthesisContext &SC,
                    const VectorTypeSynthesizer<S> &V) {
  return BuiltinVectorType::get(SC.Context, synthesizeType(SC, V.Sub),
                                V.Count);
}

/// A synthesizer which generates a metatype type.
template <class S>
struct MetatypeTypeSynthesizer {
  S Sub;
};
template <class S>
constexpr MetatypeTypeSynthesizer<S> _metatype(S sub) {
  return {sub};
}
template <class S>
Type synthesizeType(SynthesisContext &SC,
                    const MetatypeTypeSynthesizer<S> &M) {
  return MetatypeType::get(synthesizeType(SC, M.Sub));
}

/// A synthesizer which generates an existential metatype type.
template <class S>
struct ExistentialMetatypeTypeSynthesizer {
  S Sub;
};
template <class S>
constexpr ExistentialMetatypeTypeSynthesizer<S> _existentialMetatype(S sub) {
  return {sub};
}
template <class S>
Type synthesizeType(SynthesisContext &SC,
                    const ExistentialMetatypeTypeSynthesizer<S> &M) {
  return ExistentialMetatypeType::get(synthesizeType(SC, M.Sub));
}

/// A synthesizer that generates a MoveOnly wrapper of a type.
template <class S>
struct MoveOnlyTypeSynthesizer {
  S Sub;
};
template <class S>
constexpr MoveOnlyTypeSynthesizer<S> _moveOnly(S sub) {
  return {sub};
}
template <class S>
Type synthesizeType(SynthesisContext &SC, const MoveOnlyTypeSynthesizer<S> &M) {
  auto innerType = synthesizeType(SC, M.Sub)->getCanonicalType();
  return MoveOnlyType::get(innerType);
}

/// Helper types for variadic synthesis.
template <class... Ss>
struct VariadicSynthesizerStorage;

template <>
struct VariadicSynthesizerStorage<> {
  constexpr VariadicSynthesizerStorage() {}

  template <class Fn>
  void visit(Fn &&fn) const {}
};
template <class Head, class... Tail>
struct VariadicSynthesizerStorage<Head, Tail...> {
  Head head;
  VariadicSynthesizerStorage<Tail...> tail;
  constexpr VariadicSynthesizerStorage(Head head, Tail... tail)
    : head(head), tail(tail...) {}

  template <class Fn>
  void visit(Fn &&fn) const {
    fn(head);
    tail.visit(fn);
  }
};

/// A synthesizer which generates a generic type parameter.
struct TypeParamTypeSynthesizer {
  unsigned Index;
};
constexpr inline TypeParamTypeSynthesizer _typeparam(unsigned index) {
  return {index};
}
inline Type synthesizeType(SynthesisContext &SC,
                           const TypeParamTypeSynthesizer &S) {
  assert(SC.GenericParams);
  return SC.GenericParams->getParams()[S.Index]->getDeclaredInterfaceType();
}

/// Synthesize tuple type elements.
template <class S>
TupleTypeElt synthesizeTupleTypeElt(SynthesisContext &SC, S s) {
  return synthesizeType(SC, s);
}
struct CollectTupleTypeElements {
  SynthesisContext &SC;
  SmallVectorImpl<TupleTypeElt> &Elts;

  template <class S>
  void operator()(const S &s) const {
    Elts.push_back(synthesizeTupleTypeElt(SC, s));
  }
};

/// Synthesize tuple types.
template <class... Elts>
struct TupleSynthesizer {
  VariadicSynthesizerStorage<Elts...> Elements;
};
template <class... Elts>
constexpr TupleSynthesizer<Elts...> _tuple(Elts... elts) {
  return {{elts...}};
}
template <class... Elts>
Type synthesizeType(SynthesisContext &SC,
                    const TupleSynthesizer<Elts...> &tuple) {
  SmallVector<TupleTypeElt, sizeof...(Elts)> elts;
  tuple.Elements.visit(CollectTupleTypeElements{SC, elts});
  return TupleType::get(elts, SC.Context);
}

/// Synthesize parameter declarations.
template <class S>
ParamDecl *synthesizeParamDecl(SynthesisContext &SC, const S &s) {
  auto type = synthesizeType(SC, s);
  auto PD = new (SC.Context) ParamDecl(SourceLoc(), SourceLoc(),
                                       Identifier(), SourceLoc(),
                                       Identifier(), SC.DC);
  PD->setSpecifier(ParamSpecifier::Default);
  PD->setInterfaceType(type);
  PD->setImplicit();
  return PD;
}
template <class S>
FunctionType::Param synthesizeParamType(SynthesisContext &SC, const S &s) {
  auto type = synthesizeType(SC, s);
  return type;
}

/// Parameter specifiers.
template <class S>
struct SpecifiedParamSynthesizer { ParamSpecifier specifier; S sub; };
template <class G>
constexpr SpecifiedParamSynthesizer<G> _owned(G sub) {
  return {ParamSpecifier::Owned, sub};
}
template <class G>
constexpr SpecifiedParamSynthesizer<G> _inout(G sub) {
  return {ParamSpecifier::InOut, sub};
}
template <class S>
ParamDecl *synthesizeParamDecl(SynthesisContext &SC,
                               const SpecifiedParamSynthesizer<S> &s) {
  auto param = synthesizeParamDecl(SC, s.sub);
  param->setSpecifier(s.specifier);
  return param;
}
template <class S>
FunctionType::Param synthesizeParamType(SynthesisContext &SC,
                                        const SpecifiedParamSynthesizer<S> &s) {
  auto param = synthesizeParamType(SC, s.sub);
  auto flags = param.getParameterFlags();
  if (s.specifier == ParamSpecifier::Owned)
    flags = flags.withOwned(true);
  if (s.specifier == ParamSpecifier::InOut)
    flags = flags.withInOut(true);
  return param.withFlags(flags);
}

/// Synthesize a parameter list.
template <class... Params>
struct ParameterListSynthesizer {
  VariadicSynthesizerStorage<Params...> params;
};
template <class... Params>
constexpr ParameterListSynthesizer<Params...> _parameters(Params... ps) {
  return {{ps...}};
}

struct CollectParamDecls {
  SynthesisContext &SC;
  SmallVectorImpl<ParamDecl*> &Results;

  template <class S>
  void operator()(const S &s) const {
    // Found by argument-dependent lookup.
    Results.push_back(synthesizeParamDecl(SC, s));
  }
};
template <class... Params>
ParameterList *synthesizeParameterList(SynthesisContext &SC,
                    const ParameterListSynthesizer<Params...> &list) {
  SmallVector<ParamDecl*, 4> decls;
  list.params.visit(CollectParamDecls{SC, decls});
  return ParameterList::create(SC.Context, decls);
}

struct CollectParamTypes {
  SynthesisContext &SC;
  SmallVectorImpl<FunctionType::Param> &Results;

  template <class S>
  void operator()(const S &s) const {
    // Found by argument-dependent lookup.
    Results.push_back(synthesizeParamType(SC, s));
  }
};
template <class... Params>
void synthesizeParameterTypes(SynthesisContext &SC,
                        const ParameterListSynthesizer<Params...> &list,
                              SmallVectorImpl<FunctionType::Param> &types) {
  list.params.visit(CollectParamTypes{SC, types});
}

/// Synthesize function ExtInfo.
enum FunctionRepresentationSynthesizer {
  _thin,
  _thick
};
template <class S> struct ThrowsSynthesizer { S sub; };
template <class S> struct AsyncSynthesizer { S sub; };
template <class S>
constexpr ThrowsSynthesizer<S> _throws(S sub) { return {sub}; }
template <class S>
constexpr AsyncSynthesizer<S> _async(S sub) { return {sub}; }

inline ASTExtInfo synthesizeExtInfo(SynthesisContext &SC,
                                    FunctionRepresentationSynthesizer kind) {
  switch (kind) {
  case _thin: return ASTExtInfo().withRepresentation(
                                            FunctionTypeRepresentation::Thin);
  case _thick: return ASTExtInfo().withRepresentation(
                                           FunctionTypeRepresentation::Swift);
  }
}
template <class S>
ASTExtInfo synthesizeExtInfo(SynthesisContext &SC,
                             const ThrowsSynthesizer<S> &s) {
  return synthesizeExtInfo(SC, s.sub).withThrows();
}
template <class S>
ASTExtInfo synthesizeExtInfo(SynthesisContext &SC,
                             const AsyncSynthesizer<S> &s) {
  return synthesizeExtInfo(SC, s.sub).withAsync();
}

/// Synthesize a function type.
template <class ExtInfoS, class ResultS, class ParamsS>
struct FunctionTypeSynthesizer {
  ExtInfoS extInfo;
  ResultS result;
  ParamsS parameters;
};
template <class ExtInfoS, class ResultS, class ParamsS>
FunctionTypeSynthesizer<ExtInfoS, ResultS, ParamsS>
_function(ExtInfoS extInfo, ResultS result, ParamsS params) {
  return {extInfo, result, params};
}
template <class ExtInfoS, class ResultS, class ParamsS>
Type synthesizeType(SynthesisContext &SC,
             const FunctionTypeSynthesizer<ExtInfoS, ResultS, ParamsS> &fn) {
  SmallVector<FunctionType::Param, 4> paramTypes;
  synthesizeParameterTypes(SC, fn.parameters, paramTypes);
  auto extInfo = synthesizeExtInfo(SC, fn.extInfo);
  auto resultType = synthesizeType(SC, fn.result);
  return FunctionType::get(paramTypes, resultType, extInfo);
}

/// Synthesize optionals.
template <class S>
struct OptionalSynthesizer {
  S sub;
};
template <class S>
constexpr OptionalSynthesizer<S> _optional(S sub) { return {sub}; }
template <class S>
Type synthesizeType(SynthesisContext &SC, const OptionalSynthesizer<S> &s) {
  return OptionalType::get(synthesizeType(SC, s.sub));
}

} // end namespace swift

#endif
