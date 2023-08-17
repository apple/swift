
add_swift_host_library(swiftSema STATIC
  BuilderTransform.cpp
  CSApply.cpp
  CSBindings.cpp
  CSSyntacticElement.cpp
  CSGen.cpp
  CSRanking.cpp
  CSSimplify.cpp
  CSSolver.cpp
  CSStep.cpp
  CSFix.cpp
  CSDiagnostics.cpp
  CodeSynthesis.cpp
  CodeSynthesisDistributedActor.cpp
  ConstantnessSemaDiagnostics.cpp
  Constraint.cpp
  ConstraintGraph.cpp
  ConstraintLocator.cpp
  ConstraintSystem.cpp
  CompletionContextFinder.cpp
  DebuggerTestingTransform.cpp
  DerivedConformanceActor.cpp
  DerivedConformanceDistributedActor.cpp
  DerivedConformanceAdditiveArithmetic.cpp
  DerivedConformanceCaseIterable.cpp
  DerivedConformanceCodable.cpp
  DerivedConformanceCodingKey.cpp
  DerivedConformanceDifferentiable.cpp
  DerivedConformanceEquatableHashable.cpp
  DerivedConformanceComparable.cpp
  DerivedConformanceError.cpp
  DerivedConformanceRawRepresentable.cpp
  DerivedConformances.cpp
  ImportResolution.cpp
  InstrumenterSupport.cpp
  LookupVisibleDecls.cpp
  MiscDiagnostics.cpp
  PCMacro.cpp
  PlaygroundTransform.cpp
  PreCheckExpr.cpp
  ResilienceDiagnostics.cpp
  SourceLoader.cpp
  SyntacticElementTarget.cpp
  TypeCheckAccess.cpp
  TypeCheckAttr.cpp
  TypeCheckAvailability.cpp
  TypeCheckCaptures.cpp
  TypeCheckCircularity.cpp
  TypeCheckCodeCompletion.cpp
  TypeCheckConcurrency.cpp
  TypeCheckDistributed.cpp
  TypeCheckConstraints.cpp
  TypeCheckDecl.cpp
  TypeCheckDeclObjC.cpp
  TypeCheckDeclOverride.cpp
  TypeCheckDeclPrimary.cpp
  TypeCheckEffects.cpp
  TypeCheckExpr.cpp
  TypeCheckExprObjC.cpp
  TypeCheckGeneric.cpp
  TypeCheckMacros.cpp
  TypeCheckNameLookup.cpp
  TypeCheckPattern.cpp
  TypeCheckPropertyWrapper.cpp
  TypeCheckProtocol.cpp
  TypeCheckProtocolInference.cpp
  TypeCheckRegex.cpp
  TypeCheckRequestFunctions.cpp
  TypeCheckStmt.cpp
  TypeCheckStorage.cpp
  TypeCheckSwitchStmt.cpp
  TypeCheckType.cpp
  TypeChecker.cpp
  IDETypeCheckingRequests.cpp)
if(SWIFT_FORCE_OPTIMIZED_TYPECHECKER)
  if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC" OR CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
    target_compile_options(swiftSema PRIVATE /O2 /Ob2)
  else()
    target_compile_options(swiftSema PRIVATE -O3)
  endif()
endif()

target_link_libraries(swiftSema PRIVATE
  swiftAST
  swiftParse
  swiftSerialization)

if (SWIFT_SWIFT_PARSER)
  target_compile_definitions(swiftSema
    PRIVATE
    SWIFT_SWIFT_PARSER
  )
  target_link_libraries(swiftSema PRIVATE
    swiftASTGen)
endif()

set_swift_llvm_is_available(swiftSema)
