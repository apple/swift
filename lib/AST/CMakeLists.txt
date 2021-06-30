
if(SWIFT_BUILD_ONLY_SYNTAXPARSERLIB)
  set(SWIFTAST_LLVM_LINK_COMPONENTS)
else()
  set(SWIFTAST_LLVM_LINK_COMPONENTS
    bitreader bitwriter coroutines coverage irreader debuginfoDWARF
    profiledata instrumentation object objcarcopts mc mcparser
    bitreader bitwriter lto ipo option core support remarks ${LLVM_TARGETS_TO_BUILD}
  )
endif()

add_swift_host_library(swiftAST STATIC
  AbstractSourceFileDepGraphFactory.cpp
  AccessNotes.cpp
  AccessRequests.cpp
  ArgumentList.cpp
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
  DocComment.cpp
  Effects.cpp
  Evaluator.cpp
  Expr.cpp
  ExtInfo.cpp
  FineGrainedDependencies.cpp
  FineGrainedDependencyFormat.cpp
  FrontendSourceFileDepGraphFactory.cpp
  GenericEnvironment.cpp
  GenericParamList.cpp
  GenericSignature.cpp
  GenericSignatureBuilder.cpp
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
  Parameter.cpp
  Pattern.cpp
  PlatformKind.cpp
  PrettyStackTrace.cpp
  ProtocolConformance.cpp
  RawComment.cpp
  RequirementEnvironment.cpp
  RequirementMachine/ProtocolGraph.cpp
  RequirementMachine/RequirementMachine.cpp
  RequirementMachine/RewriteSystem.cpp
  RequirementMachine/RewriteSystemCompletion.cpp
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
  TypeWalker.cpp
  UnqualifiedLookup.cpp
  USRGeneration.cpp

  LLVM_LINK_COMPONENTS
  ${SWIFTAST_LLVM_LINK_COMPONENTS}
  )

if(SWIFT_FORCE_OPTIMIZED_TYPECHECKER)
  if(CMAKE_CXX_COMPILER_ID STREQUAL MSVC OR CMAKE_CXX_SIMULATE_ID STREQUAL MSVC)
    target_compile_options(swiftAST PRIVATE /O2 /Ob2)
  else()
    target_compile_options(swiftAST PRIVATE -O3)
  endif()
endif()
if(NOT SWIFT_BUILD_ONLY_SYNTAXPARSERLIB)
  target_link_libraries(swiftAST INTERFACE
    clangCodeGen
    clangIndex
    clangFormat
    clangToolingCore
    clangFrontendTool
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
endif()

target_link_libraries(swiftAST
  PUBLIC swiftBasic
  PRIVATE swiftSyntax)
if(SWIFT_BUILD_ONLY_SYNTAXPARSERLIB)
  # Remove dependencies from clangBasic to avoid bringing along some llvm
  # libraries that we don't need to be building.
  set_property(TARGET clangBasic PROPERTY LINK_LIBRARIES "")
  set_property(TARGET clangBasic PROPERTY LINK_DEPENDS "")
  set_property(TARGET clangBasic PROPERTY INTERFACE_LINK_LIBRARIES "")
  set_property(TARGET clangBasic PROPERTY INTERFACE_LINK_DEPENDS "")
  target_link_libraries(swiftAST
    PRIVATE clangBasic)
  target_compile_definitions(swiftAST PRIVATE
    SWIFT_BUILD_ONLY_SYNTAXPARSERLIB=1)
else()
  target_link_libraries(swiftAST PRIVATE
    swiftMarkup)
endif()

# intrinsics_gen is the LLVM tablegen target that generates the include files
# where intrinsics and attributes are declared. swiftAST depends on these
# headers.
# For more information see the comment at the top of lib/CMakeLists.txt.
add_dependencies(swiftAST intrinsics_gen clang-tablegen-targets)

set_swift_llvm_is_available(swiftAST)
