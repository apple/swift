
set(SWIFTAST_LLVM_LINK_COMPONENTS
  bitreader bitwriter coroutines coverage irreader debuginfoDWARF
  profiledata instrumentation object objcarcopts mc mcparser
  bitreader bitwriter lto ipo option core support remarks ${LLVM_TARGETS_TO_BUILD}
)

add_swift_host_library(swiftAST STATIC
  AbstractSourceFileDepGraphFactory.cpp
  AccessNotes.cpp
  AccessRequests.cpp
  ArgumentList.cpp
  ASTBridging.cpp
  ASTContext.cpp
  ASTDemangler.cpp
  ASTDumper.cpp
  ASTMangler.cpp
  ASTNode.cpp
  ASTPrinter.cpp
  ASTScope.cpp
  ASTScopeCreation.cpp
  ASTScopeLookup.cpp
  ASTScopePrinting.cpp
  ASTScopeSourceRange.cpp
  ASTVerifier.cpp
  ASTWalker.cpp
  Attr.cpp
  AutoDiff.cpp
  Availability.cpp
  AvailabilitySpec.cpp
  Builtins.cpp
  CASTBridging.cpp
  CaptureInfo.cpp
  ClangSwiftTypeCorrespondence.cpp
  ClangTypeConverter.cpp
  ConcreteDeclRef.cpp
  ConformanceLookupTable.cpp
  Decl.cpp
  DeclContext.cpp
  DeclNameLoc.cpp
  DiagnosticConsumer.cpp
  DiagnosticEngine.cpp
  DiagnosticList.cpp
  DistributedDecl.cpp
  DocComment.cpp
  Effects.cpp
  Evaluator.cpp
  ExistentialGeneralization.cpp
  Expr.cpp
  ExtInfo.cpp
  FineGrainedDependencies.cpp
  FineGrainedDependencyFormat.cpp
  FreestandingMacroExpansion.cpp
  FrontendSourceFileDepGraphFactory.cpp
  GenericEnvironment.cpp
  GenericParamList.cpp
  GenericSignature.cpp
  Identifier.cpp
  ImportCache.cpp
  IndexSubset.cpp
  InlinableText.cpp
  LayoutConstraint.cpp
  Module.cpp
  ModuleDependencies.cpp
  ModuleLoader.cpp
  ModuleNameLookup.cpp
  NameLookup.cpp
  NameLookupRequests.cpp
  OperatorNameLookup.cpp
  PackConformance.cpp
  PackExpansionMatcher.cpp
  ParameterPack.cpp
  Parameter.cpp
  Pattern.cpp
  PlatformKind.cpp
  PluginLoader.cpp
  PluginRegistry.cpp
  PrettyStackTrace.cpp
  ProtocolConformance.cpp
  ProtocolConformanceRef.cpp
  RawComment.cpp
  Requirement.cpp
  RequirementEnvironment.cpp
  RequirementMachine/ConcreteContraction.cpp
  RequirementMachine/ConcreteTypeWitness.cpp
  RequirementMachine/Diagnostics.cpp
  RequirementMachine/GenericSignatureQueries.cpp
  RequirementMachine/HomotopyReduction.cpp
  RequirementMachine/InterfaceType.cpp
  RequirementMachine/KnuthBendix.cpp
  RequirementMachine/MinimalConformances.cpp
  RequirementMachine/NameLookup.cpp
  RequirementMachine/NormalizeRewritePath.cpp
  RequirementMachine/PropertyMap.cpp
  RequirementMachine/PropertyRelations.cpp
  RequirementMachine/PropertyUnification.cpp
  RequirementMachine/RequirementBuilder.cpp
  RequirementMachine/RequirementLowering.cpp
  RequirementMachine/RequirementMachine.cpp
  RequirementMachine/RequirementMachineRequests.cpp
  RequirementMachine/RewriteContext.cpp
  RequirementMachine/RewriteLoop.cpp
  RequirementMachine/RewriteSystem.cpp
  RequirementMachine/Rule.cpp
  RequirementMachine/RuleBuilder.cpp
  RequirementMachine/SimplifySubstitutions.cpp
  RequirementMachine/Symbol.cpp
  RequirementMachine/Term.cpp
  RequirementMachine/TypeDifference.cpp
  SearchPathOptions.cpp
  SILLayout.cpp
  Stmt.cpp
  SubstitutionMap.cpp
  SwiftNameTranslation.cpp
  Type.cpp
  TypeCheckRequests.cpp
  TypeDeclFinder.cpp
  TypeJoinMeet.cpp
  TypeRefinementContext.cpp
  TypeRepr.cpp
  TypeSubstitution.cpp
  TypeWalker.cpp
  UnqualifiedLookup.cpp
  USRGeneration.cpp

  LLVM_LINK_COMPONENTS
  ${SWIFTAST_LLVM_LINK_COMPONENTS}
  )

if(SWIFT_FORCE_OPTIMIZED_TYPECHECKER)
  if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC" OR CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
    target_compile_options(swiftAST PRIVATE /O2 /Ob2)
  else()
    target_compile_options(swiftAST PRIVATE -O3)
  endif()
endif()
target_link_libraries(swiftAST INTERFACE
  clangCodeGen
  clangIndex
  clangFormat
  clangToolingCore
  clangFrontendTool
  clangDependencyScanning
  clangFrontend
  clangDriver
  clangSerialization
  clangParse
  clangSema
  clangAnalysis
  clangEdit
  clangRewriteFrontend
  clangRewrite
  clangAST
  clangLex
  clangAPINotes
  clangBasic)

if(SWIFT_SWIFT_PARSER)
  target_compile_definitions(swiftAST
    PRIVATE
    SWIFT_SWIFT_PARSER
  )
endif()

target_link_libraries(swiftAST
  PUBLIC swiftBasic
  PRIVATE swiftMarkup)

set_swift_llvm_is_available(swiftAST)
