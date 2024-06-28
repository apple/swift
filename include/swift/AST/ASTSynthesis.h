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
#include "swift/AST/Expr.h"
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
  _copyable,
  _error,
  _executor, // the 'BuiltinExecutor' type
  _escapable,
  _job,
  _nativeObject,
  _never,
  _rawPointer,
  _rawUnsafeContinuation,
  _void,
  _word,
  _swiftInt,       // Swift.Int
  _serialExecutor, // the '_Concurrency.SerialExecutor' protocol
  _taskExecutor,   // the '_Concurrency.TaskExecutor' protocol
  _actor,          // the '_Concurrency.Actor' protocol
  _distributedActor,  // the 'Distributed.DistributedActor' protocol
};
inline Type synthesizeType(SynthesisContext &SC,
                           SingletonTypeSynthesizer kind) {
  switch (kind) {
  case _any: return SC.Context.getAnyExistentialType();
  case _bridgeObject: return SC.Context.TheBridgeObjectType;
  case _error: return SC.Context.getErrorExistentialType();
  case _executor: return SC.Context.TheExecutorType;
  case _job: return SC.Context.TheJobType;
  case _nativeObject: return SC.Context.TheNativeObjectType;
  case _never: return SC.Context.getNeverType();
  case _rawPointer: return SC.Context.TheRawPointerType;
  case _rawUnsafeContinuation: return SC.Context.TheRawUnsafeContinuationType;
  case _void: return SC.Context.TheEmptyTupleType;
  case _word: return BuiltinIntegerType::get(BuiltinIntegerWidth::pointer(),
                                             SC.Context);
  case _swiftInt: return SC.Context.getIntType();
  case _serialExecutor:
    return SC.Context.getProtocol(KnownProtocolKind::SerialExecutor)
      ->getDeclaredInterfaceType();
  case _taskExecutor:
    if (auto ty = SC.Context.getProtocol(KnownProtocolKind::TaskExecutor)) {
      return ty->getDeclaredInterfaceType();
    } else {
      return nullptr;
    }
  case _actor:
    return SC.Context.getProtocol(KnownProtocolKind::Actor)
      ->getDeclaredInterfaceType();
  case _distributedActor:
    return SC.Context.getProtocol(KnownProtocolKind::DistributedActor)
      ->getDeclaredInterfaceType();
  case _copyable:
    return SC.Context.getProtocol(KnownProtocolKind::Copyable)
        ->getDeclaredInterfaceType();
  case _escapable:
    return SC.Context.getProtocol(KnownProtocolKind::Escapable)
        ->getDeclaredInterfaceType();
  }
}

enum RepresentationSynthesizer {
  _thin,
  _thick
};

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

template <class S>
struct RepMetatypeTypeSynthesizer {
  S Sub;
  RepresentationSynthesizer Rep;
};
template <class S>
constexpr RepMetatypeTypeSynthesizer<S>
_metatype(S sub, RepresentationSynthesizer rep ) {
  return {sub, rep};
}
template <class S>
Type synthesizeType(SynthesisContext &SC,
                    const RepMetatypeTypeSynthesizer<S> &M) {
  auto instanceType = synthesizeType(SC, M.Sub);
  return MetatypeType::get(instanceType, synthesizeMetatypeRepresentation(M.Rep));
}

/// A synthesizer which generates an existential type from a requirement type.
template <class S>
struct ExistentialTypeSynthesizer {
  S Sub;
};
template <class S>
constexpr ExistentialTypeSynthesizer<S> _existential(S sub) {
  return {sub};
}
template <class S>
Type synthesizeType(SynthesisContext &SC,
                    const ExistentialTypeSynthesizer<S> &M) {
  return ExistentialType::get(synthesizeType(SC, M.Sub));
}

/// A synthesizer which generates an existential type from a requirement type.
template <class S, class FallbackS>
struct BincompatIfTypeAvailableTypeSynthesizer {
  bool condition;
  S Sub;
  FallbackS FallbackSub;
};
template <class S, class FallbackS>
constexpr BincompatIfTypeAvailableTypeSynthesizer<S, FallbackS> _bincompatType(
    bool bincompatCondition, S sub, FallbackS fallbackSub) {
  return {bincompatCondition, sub, {fallbackSub}};
}
template <class S, class FallbackS>
Type synthesizeType(SynthesisContext &SC,
                    const BincompatIfTypeAvailableTypeSynthesizer<S, FallbackS> &M) {
  if (M.condition) {
    return synthesizeType(SC, M.Sub);
  } else {
    return synthesizeType(SC, M.FallbackSub);
  }
}

MetatypeRepresentation
inline synthesizeMetatypeRepresentation(RepresentationSynthesizer rep) {
  switch (rep) {
  case _thin: return MetatypeRepresentation::Thin;
  case _thick: return MetatypeRepresentation::Thick;
  // TOOD: maybe add _objc?
  }
  llvm_unreachable("bad kind");
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
template <class S>
struct RepExistentialMetatypeTypeSynthesizer {
  S Sub;
  RepresentationSynthesizer Rep;
};
template <class S>
constexpr RepExistentialMetatypeTypeSynthesizer<S>
_existentialMetatype(S sub, RepresentationSynthesizer rep) {
  return {sub, rep};
}
template <class S>
Type synthesizeType(SynthesisContext &SC,
                    const RepExistentialMetatypeTypeSynthesizer<S> &M) {
  return ExistentialMetatypeType::get(synthesizeType(SC, M.Sub),
                                      synthesizeMetatypeRepresentation(M.Rep));
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
  // Until we get the actual move only type, we just return the synthesized
  // type.
  return synthesizeType(SC, M.Sub);
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
ParamDecl *synthesizeParamDecl(SynthesisContext &SC, const S &s,
                               const char *label = nullptr) {
  auto argLabelIdent = (label ? SC.Context.getIdentifier(label) : Identifier());
  auto type = synthesizeType(SC, s);
  auto PD = new (SC.Context) ParamDecl(SourceLoc(), SourceLoc(),
                                       argLabelIdent, SourceLoc(),
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
  // TODO: We should probably synthesize decls to use the standard `consuming`
  // modifier, once we're certain doing so won't break anything.
  return {ParamSpecifier::LegacyOwned, sub};
}
template <class G>
constexpr SpecifiedParamSynthesizer<G> _consuming(G sub) {
  return {ParamSpecifier::Consuming, sub};
}
template <class G>
constexpr SpecifiedParamSynthesizer<G> _inout(G sub) {
  return {ParamSpecifier::InOut, sub};
}
template <class S>
ParamDecl *synthesizeParamDecl(SynthesisContext &SC,
                               const SpecifiedParamSynthesizer<S> &s,
                               const char *label = nullptr) {
  auto param = synthesizeParamDecl(SC, s.sub, label);
  param->setSpecifier(s.specifier);
  return param;
}

template <class S>
struct SendingParamSynthesizer {
  S sub;
};

template <class S>
constexpr SendingParamSynthesizer<S> _sending(S sub) {
  return {sub};
}

template <class S>
ParamDecl *synthesizeParamDecl(SynthesisContext &SC,
                               const SendingParamSynthesizer<S> &s,
                               const char *label = nullptr) {
  auto param = synthesizeParamDecl(SC, s.sub, label);
  if (SC.Context.LangOpts.hasFeature(Feature::SendingArgsAndResults))
    param->setSending();
  return param;
}

template <class S>
FunctionType::Param synthesizeParamType(SynthesisContext &SC,
                                        const SpecifiedParamSynthesizer<S> &s) {
  auto param = synthesizeParamType(SC, s.sub);
  auto flags = param.getParameterFlags();
  if (s.specifier != ParamSpecifier::Default)
    flags = flags.withValueOwnership(s.specifier);
  return param.withFlags(flags);
}

template <class S>
void synthesizeDefaultArgument(SynthesisContext &SC, const S &s,
                               ParamDecl *param) {
  synthesizeDefaultArgumentFromExpr(SC, s, param);
}
template <class S>
void synthesizeDefaultArgumentFromExpr(SynthesisContext &SC, const S &s,
                                       ParamDecl *param) {
  // FIXME: this works except that we tend to crash in diagnostics trying
  // to render the default argument if you mess up the call.
  auto expr = synthesizeExpr(SC, s);
  param->setDefaultArgumentKind(DefaultArgumentKind::Normal);
  param->setDefaultExpr(expr);
}

/// Default arguments.
template <class S, class A>
struct DefaultedSynthesizer { S sub; A arg; };
template <class S, class A>
constexpr DefaultedSynthesizer<S, A> _defaulted(S sub, A arg) {
  return {sub, arg};
}
template <class S, class A>
ParamDecl *synthesizeParamDecl(SynthesisContext &SC,
                               const DefaultedSynthesizer<S, A> &s,
                               const char *label = nullptr) {
  auto param = synthesizeParamDecl(SC, s.sub, label);
  synthesizeDefaultArgument(SC, s.arg, param);
  return param;
}
template <class S, class A>
FunctionType::Param synthesizeParamType(SynthesisContext &SC,
                                        const DefaultedSynthesizer<S, A> &s) {
  return synthesizeParamType(s.sub);
}

/// Labels.
template <class S>
struct LabelSynthesizer { const char *label; S sub; };
template <class S>
constexpr LabelSynthesizer<S> _label(const char *label, S sub) {
  return {label, sub};
}
template <class S>
ParamDecl *synthesizeParamDecl(SynthesisContext &SC,
                               const LabelSynthesizer<S> &s) {
  return synthesizeParamDecl(SC, s.sub, s.label);
}
template <class S>
FunctionType::Param synthesizeParamType(SynthesisContext &SC,
                                        const LabelSynthesizer<S> &s) {
  auto label = SC.Context.getIdentifier(s.label);
  return synthesizeParamType(SC, s.sub).withLabel(label);
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
template <class S> struct ThrowsSynthesizer { S sub; };
template <class S> struct AsyncSynthesizer { S sub; };
template <class S> struct NoescapeSynthesizer { S sub; };
template <class S> struct SendableModSynthesizer { S sub; };
template <class S>
constexpr ThrowsSynthesizer<S> _throws(S sub) { return {sub}; }
template <class S>
constexpr AsyncSynthesizer<S> _async(S sub) { return {sub}; }
template <class S>
constexpr NoescapeSynthesizer<S> _noescape(S sub) { return {sub}; }
template <class S>
constexpr SendableModSynthesizer<S> _sendable(S sub) { return {sub}; }

inline ASTExtInfo synthesizeExtInfo(SynthesisContext &SC,
                                    RepresentationSynthesizer kind) {
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
template <class S>
ASTExtInfo synthesizeExtInfo(SynthesisContext &SC,
                             const NoescapeSynthesizer<S> &s) {
  return synthesizeExtInfo(SC, s.sub).withNoEscape();
}
template <class S>
ASTExtInfo synthesizeExtInfo(SynthesisContext &SC,
                             const SendableModSynthesizer<S> &s) {
  return synthesizeExtInfo(SC, s.sub).withSendable();
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

/// Expressions.
enum SingletonExprSynthesizer {
  _nil
};
inline Expr *synthesizeExpr(SynthesisContext &SC, SingletonExprSynthesizer s) {
  switch (s) {
  case _nil:
    return new (SC.Context) NilLiteralExpr(SourceLoc(), /*implicit*/true);
  }
  llvm_unreachable("bad singleton kind");
}
inline void synthesizeDefaultArgument(SynthesisContext &SC,
                                      SingletonExprSynthesizer s,
                                      ParamDecl *param) {
  switch (s) {
  case _nil: {
    auto expr = synthesizeExpr(SC, s);
    param->setDefaultArgumentKind(DefaultArgumentKind::NilLiteral);
    param->setDefaultExpr(expr);
    return;
  }

  /*
  default:
    synthesizeDefaultArgumentFromExpr(SC, s, param);
    return;
   */
  }
}

} // end namespace swift

#endif
