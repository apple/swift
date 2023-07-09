//===--- TypeRef.cpp - TypeRef tests --------------------------------------===//
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

#include "swift/RemoteInspection/TypeRefBuilder.h"
#include "swift/Remote/MetadataReader.h"
#include "gtest/gtest.h"

using namespace swift;
using namespace reflection;

static const std::string ABC = "ABC";
static const std::string ABCD = "ABCD";
static const std::string XYZ = "XYZ";
static const std::string Empty = "";
static const std::string MyClass = "MyClass";
static const std::string NotMyClass = "NotMyClass";
static const std::string MyModule = "MyModule";
static const std::string Shmodule = "Shmodule";
static const std::string MyProtocol = "MyProtocol";
static const std::string Shmrotocol = "Shmrotocol";

static const TypeRefDecl ABC_decl = {"ABC"};
static const TypeRefDecl ABCD_decl = {"ABCD"};
static const TypeRefDecl XYZ_decl = {"XYZ"};
static const TypeRefDecl Empty_decl = {""};
static const TypeRefDecl MyClass_decl = {"MyClass"};
static const TypeRefDecl NotMyClass_decl = {"NotMyClass"};
static const TypeRefDecl MyModule_decl = {"MyModule"};
static const TypeRefDecl Shmodule_decl = {"Shmodule"};
static const TypeRefDecl MyProtocol_decl = {"MyProtocol"};
static const TypeRefDecl Shmrotocol_decl = {"Shmrotocol"};

using Param = remote::FunctionParam<const TypeRef *>;

TEST(TypeRefTest, UniqueBuiltinTypeRef) {
  TypeRefBuilder Builder(TypeRefBuilder::ForTesting);

  auto BI1 = Builder.createBuiltinType(ABC, ABC);
  auto BI2 = Builder.createBuiltinType(ABC, ABC);
  auto BI3 = Builder.createBuiltinType(ABCD, ABCD);

  EXPECT_EQ(BI1, BI2);
  EXPECT_NE(BI2, BI3);
}

TEST(TypeRefTest, UniqueNominalTypeRef) {
  TypeRefBuilder Builder(TypeRefBuilder::ForTesting);

  auto N1 = Builder.createNominalType(ABC_decl, nullptr);
  auto N2 = Builder.createNominalType(ABC_decl, nullptr);
  auto N3 = Builder.createNominalType(ABCD_decl, nullptr);

  EXPECT_EQ(N1, N2);
  EXPECT_NE(N2, N3);

  auto N4 = Builder.createNominalType(ABC_decl, N1);
  auto N5 = Builder.createNominalType(ABC_decl, N1);

  EXPECT_EQ(N4, N5);
}

TEST(TypeRefTest, UniqueBoundGenericTypeRef) {
  TypeRefBuilder Builder(TypeRefBuilder::ForTesting);

  auto GTP00 = Builder.createGenericTypeParameterType(0, 0);
  auto GTP01 = Builder.createGenericTypeParameterType(0, 1);

  auto BG1 = Builder.createBoundGenericType(ABC_decl, {}, nullptr);
  auto BG2 = Builder.createBoundGenericType(ABC_decl, {}, nullptr);
  auto BG3 = Builder.createBoundGenericType(ABCD_decl, {}, nullptr);

  EXPECT_EQ(BG1, BG2);
  EXPECT_NE(BG2, BG3);

  std::vector<const TypeRef *> GenericParams { GTP00, GTP01 };

  auto BG4 = Builder.createBoundGenericType(ABC_decl, GenericParams, nullptr);
  auto BG5 = Builder.createBoundGenericType(ABC_decl, GenericParams, nullptr);
  auto BG6 = Builder.createBoundGenericType(ABCD_decl, GenericParams, nullptr);

  EXPECT_EQ(BG4, BG5);
  EXPECT_NE(BG5, BG6);
  EXPECT_NE(BG5, BG1);
}

TEST(TypeRefTest, UniqueTupleTypeRef) {
  TypeRefBuilder Builder(TypeRefBuilder::ForTesting);

  auto N1 = Builder.createNominalType(ABC_decl, nullptr);
  auto N2 = Builder.createNominalType(XYZ_decl, nullptr);

  std::vector<const TypeRef *> Void;
  auto Void1 = Builder.createTupleType(Void, ArrayRef<StringRef>());
  auto Void2 = Builder.createTupleType(Void, ArrayRef<StringRef>());

  EXPECT_EQ(Void1, Void2);

  std::vector<const TypeRef *> Elements1 { N1, N2 };
  std::vector<const TypeRef *> Elements2 { N1, N2, N2 };

  auto T1 = Builder.createTupleType(Elements1, ArrayRef<StringRef>());
  auto T2 = Builder.createTupleType(Elements1, ArrayRef<StringRef>());
  auto T3 = Builder.createTupleType(Elements2, ArrayRef<StringRef>());

  EXPECT_EQ(T1, T2);
  EXPECT_NE(T2, T3);
  EXPECT_NE(T1, Void1);
}

TEST(TypeRefTest, UniqueFunctionTypeRef) {

  TypeRefBuilder Builder(TypeRefBuilder::ForTesting);

  std::vector<const TypeRef *> Void;
  auto VoidResult = Builder.createTupleType(Void, ArrayRef<StringRef>());
  Param Param1 = Builder.createNominalType(ABC_decl, nullptr);
  Param Param2 = Builder.createNominalType(XYZ_decl, nullptr);

  std::vector<Param> VoidParams;
  std::vector<Param> Parameters1{Param1, Param2};
  std::vector<Param> Parameters2{Param1, Param1};

  auto Result =
      Builder.createTupleType({Param1.getType(), Param2.getType()},
                              ArrayRef<StringRef>());

  auto F1 = Builder.createFunctionType(
      Parameters1, Result, FunctionTypeFlags(),
      FunctionMetadataDifferentiabilityKind::NonDifferentiable, nullptr);
  auto F2 = Builder.createFunctionType(
      Parameters1, Result, FunctionTypeFlags(),
      FunctionMetadataDifferentiabilityKind::NonDifferentiable, nullptr);
  auto F3 = Builder.createFunctionType(
      Parameters2, Result, FunctionTypeFlags(),
      FunctionMetadataDifferentiabilityKind::NonDifferentiable, nullptr);

  EXPECT_EQ(F1, F2);
  EXPECT_NE(F2, F3);

  auto F4 = Builder.createFunctionType(
      Parameters1, Result, FunctionTypeFlags().withThrows(true),
      FunctionMetadataDifferentiabilityKind::NonDifferentiable, nullptr);
  auto F5 = Builder.createFunctionType(
      Parameters1, Result, FunctionTypeFlags().withThrows(true),
      FunctionMetadataDifferentiabilityKind::NonDifferentiable, nullptr);

  EXPECT_EQ(F4, F5);
  EXPECT_NE(F4, F1);

  // Test parameter with and without inout/shared/variadic and/or label.
  ParameterFlags paramFlags;
  auto inoutFlags = paramFlags.withValueOwnership(ValueOwnership::InOut);
  auto variadicFlags = paramFlags.withVariadic(true);
  auto sharedFlags = paramFlags.withValueOwnership(ValueOwnership::Shared);
  auto ownedFlags = paramFlags.withValueOwnership(ValueOwnership::Owned);

  auto F6 = Builder.createFunctionType(
      {Param1.withFlags(inoutFlags)}, Result, FunctionTypeFlags(),
      FunctionMetadataDifferentiabilityKind::NonDifferentiable, nullptr);
  auto F6_1 = Builder.createFunctionType(
      {Param1.withFlags(inoutFlags)}, Result, FunctionTypeFlags(),
      FunctionMetadataDifferentiabilityKind::NonDifferentiable, nullptr);
  EXPECT_EQ(F6, F6_1);

  auto F7 = Builder.createFunctionType(
      {Param1.withFlags(variadicFlags)}, Result, FunctionTypeFlags(),
      FunctionMetadataDifferentiabilityKind::NonDifferentiable, nullptr);
  auto F7_1 = Builder.createFunctionType(
      {Param1.withFlags(variadicFlags)}, Result, FunctionTypeFlags(),
      FunctionMetadataDifferentiabilityKind::NonDifferentiable, nullptr);
  EXPECT_EQ(F7, F7_1);

  auto F8 = Builder.createFunctionType(
      {Param1.withFlags(sharedFlags)}, Result, FunctionTypeFlags(),
      FunctionMetadataDifferentiabilityKind::NonDifferentiable, nullptr);
  auto F8_1 = Builder.createFunctionType(
      {Param1.withFlags(sharedFlags)}, Result, FunctionTypeFlags(),
      FunctionMetadataDifferentiabilityKind::NonDifferentiable, nullptr);
  EXPECT_EQ(F8, F8_1);

  auto F9 = Builder.createFunctionType(
      {Param1.withFlags(ownedFlags)}, Result, FunctionTypeFlags(),
      FunctionMetadataDifferentiabilityKind::NonDifferentiable, nullptr);
  auto F9_1 = Builder.createFunctionType(
      {Param1.withFlags(ownedFlags)}, Result, FunctionTypeFlags(),
      FunctionMetadataDifferentiabilityKind::NonDifferentiable, nullptr);
  EXPECT_EQ(F9, F9_1);

  auto F10 = Builder.createFunctionType(
      {Param1}, Result, FunctionTypeFlags(),
      FunctionMetadataDifferentiabilityKind::NonDifferentiable, nullptr);
  auto F10_1 = Builder.createFunctionType(
      {Param1.withLabel("foo")}, Result, FunctionTypeFlags(),
      FunctionMetadataDifferentiabilityKind::NonDifferentiable, nullptr);
  EXPECT_NE(F10, F10_1);

  EXPECT_NE(F6, F7);
  EXPECT_NE(F6, F8);
  EXPECT_NE(F6, F9);
  EXPECT_NE(F6, F10);
  EXPECT_NE(F7, F8);
  EXPECT_NE(F7, F9);
  EXPECT_NE(F7, F10);
  EXPECT_NE(F8, F9);
  EXPECT_NE(F8, F10);
  EXPECT_NE(F9, F10);

  auto VoidVoid1 =
      Builder.createFunctionType(VoidParams, VoidResult, FunctionTypeFlags(),
      FunctionMetadataDifferentiabilityKind::NonDifferentiable, nullptr);
  auto VoidVoid2 =
      Builder.createFunctionType(VoidParams, VoidResult, FunctionTypeFlags(),
      FunctionMetadataDifferentiabilityKind::NonDifferentiable, nullptr);

  EXPECT_EQ(VoidVoid1, VoidVoid2);
  EXPECT_NE(VoidVoid1, F1);

  // Test escaping.
  auto F11 = Builder.createFunctionType(
      Parameters1, Result, FunctionTypeFlags().withEscaping(true),
      FunctionMetadataDifferentiabilityKind::NonDifferentiable, nullptr);
  auto F12 = Builder.createFunctionType(
      Parameters1, Result, FunctionTypeFlags().withEscaping(true),
      FunctionMetadataDifferentiabilityKind::NonDifferentiable, nullptr);
  auto F13 = Builder.createFunctionType(
      Parameters1, Result, FunctionTypeFlags().withEscaping(false),
      FunctionMetadataDifferentiabilityKind::NonDifferentiable, nullptr);
  EXPECT_EQ(F11, F12);
  EXPECT_NE(F11, F13);

  // Test sendable.
  auto F14 = Builder.createFunctionType(
      Parameters1, Result, FunctionTypeFlags().withConcurrent(true),
      FunctionMetadataDifferentiabilityKind::NonDifferentiable, nullptr);
  auto F15 = Builder.createFunctionType(
      Parameters1, Result, FunctionTypeFlags().withConcurrent(true),
      FunctionMetadataDifferentiabilityKind::NonDifferentiable, nullptr);
  auto F16 = Builder.createFunctionType(
      Parameters1, Result, FunctionTypeFlags().withConcurrent(false),
      FunctionMetadataDifferentiabilityKind::NonDifferentiable, nullptr);
  EXPECT_EQ(F14, F15);
  EXPECT_NE(F14, F16);

  // Test differentiable.
  auto F17 = Builder.createFunctionType(
      Parameters1, Result, FunctionTypeFlags().withDifferentiable(true),
      FunctionMetadataDifferentiabilityKind::Reverse, nullptr);
  auto F18 = Builder.createFunctionType(
      Parameters1, Result, FunctionTypeFlags().withDifferentiable(true),
      FunctionMetadataDifferentiabilityKind::Reverse, nullptr);
  auto F19 = Builder.createFunctionType(
      Parameters1, Result, FunctionTypeFlags().withDifferentiable(false),
      FunctionMetadataDifferentiabilityKind::Reverse, nullptr);
  EXPECT_EQ(F17, F18);
  EXPECT_NE(F17, F19);

  // Test differentiable with @noDerivative.
  {
    auto parameters = Parameters1;
    parameters[1].setNoDerivative();
    auto f1 = Builder.createFunctionType(
        parameters, Result, FunctionTypeFlags().withDifferentiable(true),
        FunctionMetadataDifferentiabilityKind::Reverse, nullptr);
    auto f2 = Builder.createFunctionType(
        parameters, Result, FunctionTypeFlags().withDifferentiable(true),
        FunctionMetadataDifferentiabilityKind::Reverse, nullptr);
    auto f3 = Builder.createFunctionType(
        Parameters1, Result, FunctionTypeFlags().withDifferentiable(true),
        FunctionMetadataDifferentiabilityKind::Reverse, nullptr);
    EXPECT_EQ(f1, f2);
    EXPECT_NE(f1, f3);
  }
}

TEST(TypeRefTest, UniqueProtocolTypeRef) {
  TypeRefBuilder Builder(TypeRefBuilder::ForTesting);

  TypeRefBuilder::BuiltProtocolDecl P1 = std::make_pair(ABC, false);
  TypeRefBuilder::BuiltProtocolDecl P2 = std::make_pair(ABC, false);
  TypeRefBuilder::BuiltProtocolDecl P3 = std::make_pair(ABCD, false);
  TypeRefBuilder::BuiltProtocolDecl P4 = std::make_pair(XYZ, false);

  EXPECT_EQ(P1, P2);
  EXPECT_NE(P2, P3);
  EXPECT_NE(P2, P3);
  EXPECT_NE(P3, P4);

  auto PC1 = Builder.createProtocolCompositionType({P1, P2}, nullptr, false);
  auto PC2 = Builder.createProtocolCompositionType({P1, P2}, nullptr, false);
  auto PC3 =
    Builder.createProtocolCompositionType({P1, P2, P2}, nullptr, false);
  auto Any = Builder.createProtocolCompositionType({}, nullptr, false);

  EXPECT_EQ(PC1, PC2);
  EXPECT_NE(PC2, PC3);
  EXPECT_NE(PC1, Any);
}

TEST(TypeRefTest, UniqueMetatypeTypeRef) {
  TypeRefBuilder Builder(TypeRefBuilder::ForTesting);

  auto N1 = Builder.createNominalType(ABC_decl, nullptr);
  auto M1 = Builder.createMetatypeType(N1, llvm::None);
  auto M2 = Builder.createMetatypeType(N1, llvm::None);
  auto MM3 = Builder.createMetatypeType(M1, llvm::None);
  auto M4 = Builder.createMetatypeType(N1, Demangle::ImplMetatypeRepresentation::Thick);

  EXPECT_EQ(M1, M2);
  EXPECT_NE(M2, MM3);
  EXPECT_NE(M1, M4);
}

TEST(TypeRefTest, UniqueExistentialMetatypeTypeRef) {
  TypeRefBuilder Builder(TypeRefBuilder::ForTesting);

  auto N1 = Builder.createNominalType(ABC_decl, nullptr);
  auto M1 = Builder.createExistentialMetatypeType(N1);
  auto M2 = Builder.createExistentialMetatypeType(N1);
  auto MM3 = Builder.createExistentialMetatypeType(M1);

  EXPECT_EQ(M1, M2);
  EXPECT_NE(M2, MM3);
}

TEST(TypeRefTest, UniqueGenericTypeParameterTypeRef) {
  TypeRefBuilder Builder(TypeRefBuilder::ForTesting);

  auto GTP00 = Builder.createGenericTypeParameterType(0, 0);
  auto GTP00_2 = Builder.createGenericTypeParameterType(0, 0);
  auto GTP01 = Builder.createGenericTypeParameterType(0, 1);
  auto GTP10 = Builder.createGenericTypeParameterType(1, 0);

  EXPECT_EQ(GTP00, GTP00_2);
  EXPECT_NE(GTP00, GTP01);
  EXPECT_NE(GTP01, GTP10);
}

TEST(TypeRefTest, UniqueDependentMemberTypeRef) {
  TypeRefBuilder Builder(TypeRefBuilder::ForTesting);

  auto N1 = Builder.createNominalType(ABC_decl, nullptr);
  auto N2 = Builder.createNominalType(XYZ_decl, nullptr);
  TypeRefBuilder::BuiltProtocolDecl P1 = std::make_pair(ABC, false);
  TypeRefBuilder::BuiltProtocolDecl P2 = std::make_pair(ABCD, false);

  auto DM1 = Builder.createDependentMemberType("Index", N1, P1);
  auto DM2 = Builder.createDependentMemberType("Index", N1, P1);
  auto DM3 = Builder.createDependentMemberType("Element", N1, P1);
  auto DM4 = Builder.createDependentMemberType("Index", N2, P1);
  auto DM5 = Builder.createDependentMemberType("Index", N2, P2);

  EXPECT_EQ(DM1, DM2);
  EXPECT_NE(DM2, DM3);
  EXPECT_NE(DM2, DM4);
  EXPECT_NE(DM4, DM5);
}

TEST(TypeRefTest, UniqueForeignClassTypeRef) {
  TypeRefBuilder Builder(TypeRefBuilder::ForTesting);

  auto UN1 = Builder.getUnnamedForeignClassType();
  auto UN2 = Builder.getUnnamedForeignClassType();
  auto FC1 = Builder.createForeignClassType(ABC);
  auto FC2 = Builder.createForeignClassType(ABC);
  auto FC3 = Builder.createForeignClassType(ABCD);

  EXPECT_EQ(UN1, UN2);
  EXPECT_EQ(FC1, FC2);
  EXPECT_NE(FC2, FC3);
}

TEST(TypeRefTest, UniqueObjCClassTypeRef) {
  TypeRefBuilder Builder(TypeRefBuilder::ForTesting);

  auto UN1 = Builder.getUnnamedObjCClassType();
  auto UN2 = Builder.getUnnamedObjCClassType();
  auto FC1 = Builder.createObjCClassType(ABC);
  auto FC2 = Builder.createObjCClassType(ABC);
  auto FC3 = Builder.createObjCClassType(ABCD);

  EXPECT_EQ(UN1, UN2);
  EXPECT_EQ(FC1, FC2);
  EXPECT_NE(FC2, FC3);
}

TEST(TypeRefTest, UniqueOpaqueTypeRef) {
  TypeRefBuilder Builder(TypeRefBuilder::ForTesting);

  auto Op = OpaqueTypeRef::get();
  auto Op1 = Builder.getOpaqueType();
  auto Op2 = Builder.getOpaqueType();

  EXPECT_EQ(Op, Op1);
  EXPECT_EQ(Op1, Op2);
}

TEST(TypeRefTest, UniqueUnownedStorageType) {
  TypeRefBuilder Builder(TypeRefBuilder::ForTesting);

  auto N1 = Builder.createNominalType(MyClass_decl, nullptr);
  auto N2 = Builder.createNominalType(NotMyClass_decl, nullptr);
  auto RS1 = Builder.createUnownedStorageType(N1);
  auto RS2 = Builder.createUnownedStorageType(N1);
  auto RS3 = Builder.createUnownedStorageType(N2);

  EXPECT_EQ(RS1, RS2);
  EXPECT_NE(RS2, RS3);
}

TEST(TypeRefTest, UniqueWeakStorageType) {
  TypeRefBuilder Builder(TypeRefBuilder::ForTesting);

  auto N1 = Builder.createNominalType(MyClass_decl, nullptr);
  auto N2 = Builder.createNominalType(NotMyClass_decl, nullptr);
  auto RS1 = Builder.createWeakStorageType(N1);
  auto RS2 = Builder.createWeakStorageType(N1);
  auto RS3 = Builder.createWeakStorageType(N2);

  EXPECT_EQ(RS1, RS2);
  EXPECT_NE(RS2, RS3);
}

TEST(TypeRefTest, UniqueUnmanagedStorageType) {
  TypeRefBuilder Builder(TypeRefBuilder::ForTesting);

  auto N1 = Builder.createNominalType(MyClass_decl, nullptr);
  auto N2 = Builder.createNominalType(NotMyClass_decl, nullptr);
  auto RS1 = Builder.createUnmanagedStorageType(N1);
  auto RS2 = Builder.createUnmanagedStorageType(N1);
  auto RS3 = Builder.createUnmanagedStorageType(N2);

  EXPECT_EQ(RS1, RS2);
  EXPECT_NE(RS2, RS3);
}

// Tests that a concrete ABC<Int, Int> is the exact same typeref
// (with the same pointer) as an ABC<T, U> that got substituted
// with T : Int and U : Int.
TEST(TypeRefTest, UniqueAfterSubstitution) {
  TypeRefBuilder Builder(TypeRefBuilder::ForTesting);

  TypeRefDecl MangledIntName("Si");
  auto NominalInt = Builder.createNominalType(MangledIntName,
                                              /*parent*/ nullptr);
  std::vector<const TypeRef *> ConcreteArgs { NominalInt, NominalInt };

  TypeRefDecl MangledName("ABC");

  auto ConcreteBG = Builder.createBoundGenericType(MangledName,
                                                   ConcreteArgs,
                                                   /*parent*/ nullptr);

  auto GTP00 = Builder.createGenericTypeParameterType(0, 0);
  auto GTP01 = Builder.createGenericTypeParameterType(0, 1);
  std::vector<const TypeRef *> GenericParams { GTP00, GTP01 };

  auto Unbound = Builder.createBoundGenericType(MangledName, GenericParams,
                                                /*parent*/ nullptr);

  GenericArgumentMap Subs;
  Subs[{0,0}] = NominalInt;
  Subs[{0,1}] = NominalInt;

  auto SubstitutedBG = Unbound->subst(Builder, Subs);

  EXPECT_EQ(ConcreteBG, SubstitutedBG);
}

// Make sure subst() and isConcrete() walk into parent types
TEST(TypeRefTest, NestedTypes) {
  TypeRefBuilder Builder(TypeRefBuilder::ForTesting);

  auto GTP00 = Builder.createGenericTypeParameterType(0, 0);

  TypeRefDecl ParentName("parent");
  std::vector<const TypeRef *> ParentArgs { GTP00 };
  auto Parent = Builder.createBoundGenericType(ParentName, ParentArgs,
                                               /*parent*/ nullptr);

  TypeRefDecl ChildName("child");
  auto Child = Builder.createNominalType(ChildName, Parent);

  EXPECT_FALSE(Child->isConcrete());

  TypeRefDecl SubstName("subst");
  auto SubstArg = Builder.createNominalType(SubstName, /*parent*/ nullptr);

  std::vector<const TypeRef *> SubstParentArgs { SubstArg };
  auto SubstParent = Builder.createBoundGenericType(ParentName,
                                                    SubstParentArgs,
                                                    /*parent*/ nullptr);
  auto SubstChild = Builder.createNominalType(ChildName, SubstParent);

  GenericArgumentMap Subs;
  Subs[{0,0}] = SubstArg;

  EXPECT_TRUE(Child->isConcreteAfterSubstitutions(Subs));
  EXPECT_EQ(SubstChild, Child->subst(Builder, Subs));
}

TEST(TypeRefTest, DeriveSubstitutions) {
  TypeRefBuilder Builder(TypeRefBuilder::ForTesting);

  auto GTP00 = Builder.createGenericTypeParameterType(0, 0);
  auto GTP01 = Builder.createGenericTypeParameterType(0, 1);

  TypeRefDecl NominalName("nominal");
  std::vector<const TypeRef *> NominalArgs { GTP00 };
  auto Nominal = Builder.createBoundGenericType(NominalName, NominalArgs,
                                               /*parent*/ nullptr);

  auto Result = Builder.createTupleType({GTP00, GTP01},
                                        ArrayRef<StringRef>());
  auto Func = Builder.createFunctionType(
      {Nominal}, Result, FunctionTypeFlags(),
      FunctionMetadataDifferentiabilityKind::NonDifferentiable, nullptr);

  TypeRefDecl SubstOneName("subst1");
  auto SubstOne = Builder.createNominalType(SubstOneName, /*parent*/ nullptr);

  TypeRefDecl SubstTwoName("subst2");
  auto SubstTwo = Builder.createNominalType(SubstTwoName, /*parent*/ nullptr);

  GenericArgumentMap Subs;
  Subs[{0,0}] = SubstOne;
  Subs[{0,1}] = SubstTwo;

  auto Subst = Func->subst(Builder, Subs);

  GenericArgumentMap DerivedSubs;
  EXPECT_TRUE(TypeRef::deriveSubstitutions(DerivedSubs, Func, Subst));

  auto ResultOne = DerivedSubs[{0,0}];
  auto ResultTwo = DerivedSubs[{0,1}];
  EXPECT_EQ(SubstOne, ResultOne);
  EXPECT_EQ(SubstTwo, ResultTwo);
}
