
add_swift_host_library(swiftSILGen STATIC
  ArgumentSource.cpp
  Cleanup.cpp
  Condition.cpp
  FormalEvaluation.cpp
  ManagedValue.cpp
  ResultPlan.cpp
  RValue.cpp
  Scope.cpp
  SwitchEnumBuilder.cpp
  SILGen.cpp
  SILGenApply.cpp
  SILGenBackDeploy.cpp
  SILGenBridging.cpp
  SILGenBuilder.cpp
  SILGenBuiltin.cpp
  SILGenConstructor.cpp
  SILGenConvert.cpp
  SILGenDecl.cpp
  SILGenDestructor.cpp
  SILGenDistributed.cpp
  SILGenDynamicCast.cpp
  SILGenEpilog.cpp
  SILGenExpr.cpp
  SILGenForeignError.cpp
  SILGenFunction.cpp
  SILGenGlobalVariable.cpp
  SILGenLazyConformance.cpp
  SILGenLValue.cpp
  SILGenPack.cpp
  SILGenPattern.cpp
  SILGenPoly.cpp
  SILGenProlog.cpp
  SILGenRequests.cpp
  SILGenStmt.cpp
  SILGenThunk.cpp
  SILGenTopLevel.cpp
  SILGenType.cpp)
target_link_libraries(swiftSILGen PRIVATE
  swiftSerialization
  swiftSIL)

set_swift_llvm_is_available(swiftSILGen)
