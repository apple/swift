//===--- GenericSignatureQueries.cpp --------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2021 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// The various generic signature query operations on GenericSignature will
// lazily construct a requirement machine for the generic signature from the
// RewriteContext, then call the methods in this file.
//
// If you're working elsewhere in the compiler, use the methods on
// GenericSignature instead of calling into the RequirementMachine directly.
//
// Each query is generally implemented in the same manner:
//
// - First, convert the subject type parameter into a Term.
// - Simplify the Term to obtain a reduced Term.
// - Perform a property map lookup on the Term.
// - Return the appropriate piece of information from the property map.
//
// A few are slightly different; for example, getReducedType() takes an
// arbitrary type, not just a type parameter, and recursively transfozms the
// type parameters it contains, if any.
//
// Also, getConformancePath() is another one-off operation.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/ASTContext.h"
#include "swift/AST/Decl.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/GenericSignature.h"
#include "swift/AST/Module.h"
#include <vector>
#include "NameLookup.h"
#include "RequirementMachine.h"

using namespace swift;
using namespace rewriting;

/// Collects all requirements on a type parameter that are used to construct
/// its ArchetypeType in a GenericEnvironment.
GenericSignature::LocalRequirements
RequirementMachine::getLocalRequirements(
    Type depType,
    ArrayRef<GenericTypeParamType *> genericParams) const {
  auto term = Context.getMutableTermForType(depType->getCanonicalType(),
                                            /*proto=*/nullptr);
  System.simplify(term);
  verify(term);

  GenericSignature::LocalRequirements result;
  result.anchor = Map.getTypeForTerm(term, genericParams);
  result.packShape = getReducedShape(depType, genericParams);

  auto *props = Map.lookUpProperties(term);
  if (!props)
    return result;

  if (props->isConcreteType()) {
    result.concreteType = props->getConcreteType({}, term, Map);
    return result;
  }

  if (props->hasSuperclassBound()) {
    result.superclass = props->getSuperclassBound({}, term, Map);
  }

  for (const auto *proto : props->getConformsTo())
    result.protos.push_back(const_cast<ProtocolDecl *>(proto));

  result.layout = props->getLayoutConstraint();

  return result;
}

bool RequirementMachine::requiresClass(Type depType) const {
  auto term = Context.getMutableTermForType(depType->getCanonicalType(),
                                            /*proto=*/nullptr);
  System.simplify(term);
  verify(term);

  auto *props = Map.lookUpProperties(term);
  if (!props)
    return false;

  if (props->isConcreteType())
    return false;

  auto layout = props->getLayoutConstraint();
  return (layout && layout->isClass());
}

LayoutConstraint RequirementMachine::getLayoutConstraint(Type depType) const {
  auto term = Context.getMutableTermForType(depType->getCanonicalType(),
                                            /*proto=*/nullptr);
  System.simplify(term);
  verify(term);

  auto *props = Map.lookUpProperties(term);
  if (!props)
    return LayoutConstraint();

  return props->getLayoutConstraint();
}

bool RequirementMachine::requiresProtocol(Type depType,
                                          const ProtocolDecl *proto) const {
  auto term = Context.getMutableTermForType(depType->getCanonicalType(),
                                            /*proto=*/nullptr);
  System.simplify(term);
  verify(term);

  auto *props = Map.lookUpProperties(term);
  if (!props)
    return false;

  if (props->isConcreteType())
    return false;

  for (auto *otherProto : props->getConformsTo()) {
    if (otherProto == proto)
      return true;
  }

  return false;
}

GenericSignature::RequiredProtocols
RequirementMachine::getRequiredProtocols(Type depType) const {
  auto term = Context.getMutableTermForType(depType->getCanonicalType(),
                                            /*proto=*/nullptr);
  System.simplify(term);
  verify(term);

  auto *props = Map.lookUpProperties(term);
  if (!props)
    return { };

  if (props->isConcreteType())
    return { };

  GenericSignature::RequiredProtocols result;
  for (auto *otherProto : props->getConformsTo()) {
    result.push_back(const_cast<ProtocolDecl *>(otherProto));
  }

  ProtocolType::canonicalizeProtocols(result);

  return result;
}

Type RequirementMachine::
getSuperclassBound(Type depType,
                   ArrayRef<GenericTypeParamType *> genericParams) const {
  auto term = Context.getMutableTermForType(depType->getCanonicalType(),
                                            /*proto=*/nullptr);
  System.simplify(term);
  verify(term);

  auto *props = Map.lookUpProperties(term);
  if (!props)
    return Type();

  if (!props->hasSuperclassBound())
    return Type();

  return props->getSuperclassBound(genericParams, term, Map);
}

/// Unlike the other queries, we have occasion to call this on a requirement
/// machine for a protocol connected component as well as a top-level
/// generic signature, so plumb through the protocol to use for the root
/// `Self` generic parameter here.
bool RequirementMachine::isConcreteType(Type depType,
                                        const ProtocolDecl *proto) const {
  auto term = Context.getMutableTermForType(depType->getCanonicalType(),
                                            proto);
  System.simplify(term);
  verify(term);

  auto *props = Map.lookUpProperties(term);
  if (!props)
    return false;

  return props->isConcreteType();
}

/// Unlike the other queries, we have occasion to call this on a requirement
/// machine for a protocol connected component as well as a top-level
/// generic signature, so plumb through the protocol to use for the root
/// `Self` generic parameter here.
Type RequirementMachine::
getConcreteType(Type depType,
                ArrayRef<GenericTypeParamType *> genericParams,
                const ProtocolDecl *proto) const {
  auto term = Context.getMutableTermForType(depType->getCanonicalType(),
                                            proto);
  System.simplify(term);
  verify(term);

  auto *props = Map.lookUpProperties(term);
  if (!props)
    return Type();

  if (!props->isConcreteType())
    return Type();

  return props->getConcreteType(genericParams, term, Map);
}

bool RequirementMachine::areReducedTypeParametersEqual(Type depType1,
                                                       Type depType2) const {
  auto term1 = Context.getMutableTermForType(depType1->getCanonicalType(),
                                             /*proto=*/nullptr);
  System.simplify(term1);
  verify(term1);

  auto term2 = Context.getMutableTermForType(depType2->getCanonicalType(),
                                             /*proto=*/nullptr);
  System.simplify(term2);
  verify(term2);

  return (term1 == term2);
}

MutableTerm
RequirementMachine::getLongestValidPrefix(const MutableTerm &term) const {
  MutableTerm prefix;

  for (auto symbol : term) {
    switch (symbol.getKind()) {
    case Symbol::Kind::Name:
      return prefix;

    case Symbol::Kind::Protocol:
      assert(prefix.empty() &&
             "Protocol symbol can only appear at the start of a type term");
      break;

    case Symbol::Kind::GenericParam: {
      assert(prefix.empty() &&
             "Generic parameter symbol can only appear at the start of a type term");

      if (std::find_if(Params.begin(), Params.end(),
                       [&](Type otherParam) -> bool {
                         return otherParam->isEqual(symbol.getGenericParam());
                       }) == Params.end()) {
        return prefix;
      }

      break;
    }

    case Symbol::Kind::AssociatedType: {
      const auto *props = Map.lookUpProperties(prefix);
      if (!props)
        return prefix;

      auto conformsTo = props->getConformsTo();

      // T.[P:A] is valid iff T conforms to P.
      if (std::find(conformsTo.begin(), conformsTo.end(), symbol.getProtocol())
            == conformsTo.end())
        return prefix;

      break;
    }

    case Symbol::Kind::Layout:
    case Symbol::Kind::Superclass:
    case Symbol::Kind::ConcreteType:
    case Symbol::Kind::ConcreteConformance:
    case Symbol::Kind::Shape:
      llvm::errs() <<"Invalid symbol in a type term: " << term << "\n";
      abort();
    }

    // This symbol is valid, add it to the longest prefix.
    prefix.add(symbol);
  }

  return prefix;
}

/// Unlike most other queries, the input type can be any type, not just a
/// type parameter.
///
/// Returns true if all structural components that are type parameters are
/// reduced, and in particular not concrete (in which case they're not
/// considered reduced, since they can be replaced with their concrete type).
bool RequirementMachine::isReducedType(Type type) const {
  // Look for non-reduced type parameters.
  class Walker : public TypeWalker {
    const RequirementMachine &Self;

  public:
    explicit Walker(const RequirementMachine &self) : Self(self) {}

    Action walkToTypePre(Type component) override {
      if (!component->hasTypeParameter())
        return Action::SkipChildren;

      if (!component->isTypeParameter())
        return Action::Continue;

      auto term = Self.Context.getMutableTermForType(
          component->getCanonicalType(),
          /*proto=*/nullptr);

      Self.System.simplify(term);
      Self.verify(term);

      auto anchor = Self.Map.getTypeForTerm(term, {});
      if (CanType(anchor) != CanType(component))
        return Action::Stop;

      auto *props = Self.Map.lookUpProperties(term);
      if (props && props->isConcreteType())
        return Action::Stop;

      // The parent of a reduced type parameter might be non-reduced
      // because it is concrete.
      return Action::SkipChildren;
    }
  };

  return !type.walk(Walker(*this));
}

/// Given a type parameter 'T.A1.A2...An', a suffix length m where m <= n,
/// and a replacement type U, produce the type 'U.A(n-m)...An' by replacing
/// 'T.A1...A(n-m-1)' with 'U'.
static Type substPrefixType(Type type, unsigned suffixLength, Type prefixType,
                            GenericSignature sig) {
  if (suffixLength == 0)
    return prefixType;

  auto *memberType = type->castTo<DependentMemberType>();
  auto substBaseType = substPrefixType(memberType->getBase(), suffixLength - 1,
                                       prefixType, sig);
  return memberType->substBaseType(substBaseType,
                                   LookUpConformanceInSignature(sig.getPointer()));
}

/// Unlike most other queries, the input type can be any type, not just a
/// type parameter.
///
/// Replaces all structural components that are type parameters with their
/// reduced form, which is either a (possibly different) type parameter,
/// or a concrete type, in which case we recursively reduce any type
/// parameters appearing in structural positions of that concrete type
/// as well, and so on.
Type RequirementMachine::getReducedType(
    Type type,
    ArrayRef<GenericTypeParamType *> genericParams) const {

  return type.transformRec([&](Type t) -> llvm::Optional<Type> {
    if (!t->hasTypeParameter())
      return t;

    // The reduced type of a PackExpansionType has a reduced *shape* for
    // the count type.
    if (auto *packExpansionType = t->getAs<PackExpansionType>()) {
      auto reducedPattern = getReducedType(packExpansionType->getPatternType(),
                                           genericParams);
      auto reducedShape = packExpansionType->getCountType();
      if (reducedShape->isParameterPack())
        reducedShape = getReducedShape(reducedShape, genericParams);
      return Type(PackExpansionType::get(reducedPattern, reducedShape));
    }

    if (!t->isTypeParameter())
      return llvm::None;

    // Get a simplified term T.
    auto term = Context.getMutableTermForType(t->getCanonicalType(),
                                              /*proto=*/nullptr);
    System.simplify(term);

    // We need to handle "purely concrete" member types, eg if I have a
    // signature <T where T == Foo>, and we're asked to reduce the
    // type T.[P:A] where Foo : A.
    //
    // This comes up because we can derive the signature <T where T == Foo>
    // from a generic signature like <T where T : P>; adding the
    // concrete requirement 'T == Foo' renders 'T : P' redundant. We then
    // want to take interface types written against the original signature
    // and reduce them with respect to the derived signature.
    //
    // The problem is that T.[P:A] is not a valid term in the rewrite system
    // for <T where T == Foo>, since we do not have the requirement T : P.
    //
    // A more principled solution would build a substitution map when
    // building a derived generic signature that adds new requirements;
    // interface types would first be substituted before being reduced
    // in the new signature.
    //
    // For now, we handle this with a two-step process; we split a term up
    // into a longest valid prefix, which must resolve to a concrete type,
    // and the remaining suffix, which we use to perform a concrete
    // substitution using subst().

    // In the below, let T be a type term, with T == UV, where U is the
    // longest valid prefix.
    //
    // Note that V can be empty if T is fully valid; we expect this to be
    // true most of the time.
    auto prefix = getLongestValidPrefix(term);

    // Get a type (concrete or dependent) for U.
    auto prefixType = [&]() -> Type {
      verify(prefix);

      auto *props = Map.lookUpProperties(prefix);
      if (props) {
        if (props->isConcreteType()) {
          auto concreteType = props->getConcreteType(genericParams,
                                                     prefix, Map);
          if (!concreteType->hasTypeParameter())
            return concreteType;

          // FIXME: Recursion guard is needed here
          return getReducedType(concreteType, genericParams);
        }

        // Skip this part if the entire input term is valid, because in that
        // case we don't want to replace the term with its superclass bound;
        // unlike a fixed concrete type, the superclass bound only comes into
        // play when looking up a member type.
        if (props->hasSuperclassBound() &&
            prefix.size() != term.size()) {
          auto superclass = props->getSuperclassBound(genericParams,
                                                      prefix, Map);
          if (!superclass->hasTypeParameter())
            return superclass;

          // FIXME: Recursion guard is needed here
          return getReducedType(superclass, genericParams);
        }
      }

      return Map.getTypeForTerm(prefix, genericParams);
    }();

    // If T is already valid, the longest valid prefix U of T is T itself, and
    // V is empty. Just return the type we computed above.
    //
    // This is the only case where U is allowed to be dependent.
    if (prefix.size() == term.size())
      return prefixType;

    // If U is not concrete, we have an invalid member type of a dependent
    // type, which is not valid in this generic signature. Give up.
    if (prefixType->isTypeParameter()) {
      llvm::errs() << "Invalid type parameter in getReducedType()\n";
      llvm::errs() << "Original type: " << type << "\n";
      llvm::errs() << "Simplified term: " << term << "\n";
      llvm::errs() << "Longest valid prefix: " << prefix << "\n";
      llvm::errs() << "Prefix type: " << prefixType << "\n";
      llvm::errs() << "\n";
      dump(llvm::errs());
      abort();
    }

    // Compute the type of the unresolved suffix term V.
    auto substType = substPrefixType(t, term.size() - prefix.size(),
                                     prefixType, Sig);

    // FIXME: Recursion guard is needed here
    return getReducedType(substType, genericParams);
  });
}

/// Determine if the given type parameter is valid with respect to this
/// requirement machine's generic signature.
bool RequirementMachine::isValidTypeParameter(Type type) const {
  assert(type->isTypeParameter());

  auto term = Context.getMutableTermForType(type->getCanonicalType(),
                                            /*proto=*/nullptr);
  System.simplify(term);

  auto prefix = getLongestValidPrefix(term);
  return (prefix == term);
}

/// Retrieve the conformance path used to extract the conformance of
/// interface \c type to the given \c protocol.
///
/// \param type The interface type whose conformance access path is to be
/// queried.
/// \param protocol A protocol to which \c type conforms.
///
/// \returns the conformance access path that starts at a requirement of
/// this generic signature and ends at the conformance that makes \c type
/// conform to \c protocol.
///
/// \seealso ConformancePath
ConformancePath
RequirementMachine::getConformancePath(Type type,
                                       ProtocolDecl *protocol) {
  assert(type->isTypeParameter());

  auto mutTerm = Context.getMutableTermForType(type->getCanonicalType(),
                                               /*proto=*/nullptr);
  System.simplify(mutTerm);
  verify(mutTerm);

#ifndef NDEBUG
  auto *props = Map.lookUpProperties(mutTerm);
  assert(props &&
         "Subject type of conformance access path should be known");
  assert(!props->isConcreteType() &&
         "Concrete types do not have conformance access paths");
  auto conformsTo = props->getConformsTo();
  assert(std::find(conformsTo.begin(), conformsTo.end(), protocol) &&
         "Subject type of conformance access path must conform to protocol");
#endif

  auto term = Term::get(mutTerm, Context);

  // Check if we've already cached the result before doing anything else.
  auto found = ConformancePaths.find(
      std::make_pair(term, protocol));
  if (found != ConformancePaths.end()) {
    return found->second;
  }

  auto &ctx = Context.getASTContext();

  FrontendStatsTracer tracer(Stats, "get-conformance-access-path");

  auto recordPath = [&](Term term, ProtocolDecl *proto,
                        ConformancePath path) {
    // Add the path to the buffer.
    CurrentConformancePaths.emplace_back(term, path);

    // Add the path to the map.
    auto key = std::make_pair(term, proto);
    auto inserted = ConformancePaths.insert(
        std::make_pair(key, path));
    assert(inserted.second);
    (void) inserted;

    if (Stats)
      ++Stats->getFrontendCounters().NumConformancePathsRecorded;
  };

  // If this is the first time we're asked to look up a conformance access path,
  // visit all of the root conformance requirements in our generic signature and
  // add them to the buffer.
  if (ConformancePaths.empty()) {
    for (const auto &req : Sig.getRequirements()) {
      // We only care about conformance requirements.
      if (req.getKind() != RequirementKind::Conformance)
        continue;

      auto rootType = CanType(req.getFirstType());
      auto *rootProto = req.getProtocolDecl();

      ConformancePath::Entry root(rootType, rootProto);
      ArrayRef<ConformancePath::Entry> path(root);
      ConformancePath result(ctx.AllocateCopy(path));

      auto mutTerm = Context.getMutableTermForType(rootType, nullptr);
      System.simplify(mutTerm);

      auto rootTerm = Term::get(mutTerm, Context);
      recordPath(rootTerm, rootProto, result);
    }
  }

  // We enumerate conformance paths in shortlex order until we find the
  // path whose corresponding type reduces to the one we are looking for.
  while (true) {
    auto found = ConformancePaths.find(
        std::make_pair(term, protocol));
    if (found != ConformancePaths.end()) {
      return found->second;
    }

    if (CurrentConformancePaths.empty()) {
      llvm::errs() << "Failed to find conformance path for ";
      llvm::errs() << type << " (" << term << ")" << " : ";
      llvm::errs() << protocol->getName() << ":\n";
      type.dump(llvm::errs());
      llvm::errs() << "\n";
      dump(llvm::errs());
      abort();
    }

    // The buffer consists of all conformance paths of length N.
    // Swap it out with an empty buffer, and fill it with all paths of
    // length N+1.
    std::vector<std::pair<Term, ConformancePath>> oldPaths;
    std::swap(CurrentConformancePaths, oldPaths);

    for (const auto &pair : oldPaths) {
      const auto &lastElt = pair.second.back();
      auto *lastProto = lastElt.second;

      // A copy of the current path, populated as needed.
      SmallVector<ConformancePath::Entry, 4> entries;

      auto reqs = lastProto->getRequirementSignature().getRequirements();
      for (const auto &req : reqs) {
        // We only care about conformance requirements.
        if (req.getKind() != RequirementKind::Conformance)
          continue;

        auto nextSubjectType = req.getFirstType()->getCanonicalType();
        auto *nextProto = req.getProtocolDecl();

        MutableTerm mutTerm(pair.first);
        mutTerm.append(Context.getMutableTermForType(nextSubjectType,
                                                     /*proto=*/lastProto));
        System.simplify(mutTerm);

        auto nextTerm = Term::get(mutTerm, Context);

        // If we've already seen a path for this conformance, skip it and
        // don't add it to the buffer. Note that because we iterate over
        // conformance access paths in shortlex order, the existing
        // conformance access path is shorter than the one we found just now.
        if (ConformancePaths.count(
                std::make_pair(nextTerm, nextProto)))
          continue;

        if (entries.empty()) {
          // Fill our temporary vector.
          entries.insert(entries.begin(),
                         pair.second.begin(),
                         pair.second.end());
        }

        // Add the next entry.
        entries.emplace_back(nextSubjectType, nextProto);
        ConformancePath result = ctx.AllocateCopy(entries);
        entries.pop_back();

        recordPath(nextTerm, nextProto, result);
      }
    }
  }
}

TypeDecl *
RequirementMachine::lookupNestedType(Type depType, Identifier name) const {
  auto term = Context.getMutableTermForType(depType->getCanonicalType(),
                                            /*proto=*/nullptr);
  System.simplify(term);
  verify(term);

  auto *props = Map.lookUpProperties(term);
  if (!props)
    return nullptr;

  // Look for types with the given name in protocols that we know about.
  AssociatedTypeDecl *bestAssocType = nullptr;
  SmallVector<TypeDecl *, 4> concreteDecls;

  for (const auto *proto : props->getConformsTo()) {
    // Look for an associated type and/or concrete type with this name.
    for (auto member : const_cast<ProtocolDecl *>(proto)->lookupDirect(name)) {
      // If this is an associated type, record whether it is the best
      // associated type we've seen thus far.
      if (auto assocType = dyn_cast<AssociatedTypeDecl>(member)) {
        // Retrieve the associated type anchor.
        assocType = assocType->getAssociatedTypeAnchor();

        if (!bestAssocType ||
             compareAssociatedTypes(assocType, bestAssocType) < 0)
          bestAssocType = assocType;

        continue;
      }

      // If this is another type declaration, record it.
      if (auto type = dyn_cast<TypeDecl>(member)) {
        concreteDecls.push_back(type);
        continue;
      }
    }
  }

  // If we haven't found anything yet but have a concrete type or a superclass,
  // look for a type in that.
  // FIXME: Shouldn't we always look here?
  if (!bestAssocType && concreteDecls.empty()) {
    Type typeToSearch;
    if (props->isConcreteType())
      typeToSearch = props->getConcreteType();
    else if (props->hasSuperclassBound())
      typeToSearch = props->getSuperclassBound();

    if (typeToSearch)
      lookupConcreteNestedType(typeToSearch, name, concreteDecls);
  }

  if (bestAssocType) {
    assert(bestAssocType->getOverriddenDecls().empty() &&
           "Lookup should never keep a non-anchor associated type");
    return bestAssocType;

  } else if (!concreteDecls.empty()) {
    // Find the best concrete type.
    return findBestConcreteNestedType(concreteDecls);
  }

  return nullptr;
}

MutableTerm
RequirementMachine::getReducedShapeTerm(Type type) const {
  assert(type->isParameterPack());

  auto term = Context.getMutableTermForType(type->getCanonicalType(),
                                            /*proto=*/nullptr);

  // From a type term T, form the shape term `T.[shape]`.
  term.add(Symbol::forShape(Context));

  // Compute the reduced shape term `T'.[shape]`.
  System.simplify(term);
  verify(term);

  // Get the term T', which is the reduced shape of T.
  if (term.size() != 2 ||
      term[0].getKind() != Symbol::Kind::GenericParam ||
      term[1].getKind() != Symbol::Kind::Shape) {
    llvm::errs() << "Invalid reduced shape\n";
    llvm::errs() << "Type: " << type << "\n";
    llvm::errs() << "Term: " << term << "\n";
    abort();
  }

  MutableTerm reducedTerm(term.begin(), term.end() - 1);
  return reducedTerm;
}

Type RequirementMachine::getReducedShape(Type type,
                      ArrayRef<GenericTypeParamType *> genericParams) const {
  if (!type->isParameterPack())
    return Type();

  return Map.getTypeForTerm(getReducedShapeTerm(type), genericParams);
}

bool RequirementMachine::haveSameShape(Type type1, Type type2) const {
  auto term1 = getReducedShapeTerm(type1);
  auto term2 = getReducedShapeTerm(type2);

  return term1 == term2;
}

void RequirementMachine::verify(const MutableTerm &term) const {
#ifndef NDEBUG
  // If the term is in the generic parameter domain, ensure we have a valid
  // generic parameter.
  if (term.begin()->getKind() == Symbol::Kind::GenericParam) {
    auto *genericParam = term.begin()->getGenericParam();
    auto genericParams = getGenericParams();
    auto found = std::find_if(genericParams.begin(),
                              genericParams.end(),
                              [&](GenericTypeParamType *otherType) {
                                return genericParam->isEqual(otherType);
                              });
    if (found == genericParams.end()) {
      llvm::errs() << "Bad generic parameter in " << term << "\n";
      dump(llvm::errs());
      abort();
    }
  }

  MutableTerm erased;

  // First, "erase" resolved associated types from the term, and try
  // to simplify it again.
  for (auto symbol : term) {
    if (erased.empty()) {
      switch (symbol.getKind()) {
      case Symbol::Kind::Protocol:
      case Symbol::Kind::GenericParam:
        erased.add(symbol);
        continue;

      case Symbol::Kind::AssociatedType:
        erased.add(Symbol::forProtocol(symbol.getProtocol(), Context));
        break;

      case Symbol::Kind::Name:
      case Symbol::Kind::Layout:
      case Symbol::Kind::Superclass:
      case Symbol::Kind::ConcreteType:
      case Symbol::Kind::ConcreteConformance:
      case Symbol::Kind::Shape:
        llvm::errs() << "Bad initial symbol in " << term << "\n";
        abort();
        break;
      }
    }

    switch (symbol.getKind()) {
    case Symbol::Kind::Name:
      assert(!erased.empty());
      erased.add(symbol);
      break;

    case Symbol::Kind::AssociatedType:
      erased.add(Symbol::forName(symbol.getName(), Context));
      break;

    case Symbol::Kind::Shape:
      erased.add(symbol);
      break;

    case Symbol::Kind::Protocol:
    case Symbol::Kind::GenericParam:
    case Symbol::Kind::Layout:
    case Symbol::Kind::Superclass:
    case Symbol::Kind::ConcreteType:
    case Symbol::Kind::ConcreteConformance:
      llvm::errs() << "Bad interior symbol " << symbol << " in " << term << "\n";
      abort();
      break;
    }
  }

  MutableTerm simplified = erased;
  System.simplify(simplified);

  // We should end up with the same term.
  if (simplified != term) {
    llvm::errs() << "Term verification failed\n";
    llvm::errs() << "Initial term:    " << term << "\n";
    llvm::errs() << "Erased term:     " << erased << "\n";
    llvm::errs() << "Simplified term: " << simplified << "\n";
    llvm::errs() << "\n";
    dump(llvm::errs());
    abort();
  }
#endif
}
