// RUN: %target-typecheck-verify-swift -I %S/Inputs/can-import-submodule-missing-requirement/ 

#if canImport(A.BMissingRequirement)
#error("should not can import A.BMissingRequirement")
#endif

#if canImport(A.B.CMissingRequirement)
#error("should not can import A.B.CMissingRequirement")
#endif


#if canImport(A.Private.BPMissingRequirement)
#error("should not can import A.Private.BPMissingRequirement")
#endif

#if canImport(A_Private.BPMissingRequirement)
#error("should not can import A_Private.BPMissingRequirement")
#endif

#if canImport(A_Private.BP.CPMissingRequirement)
#error("should not can import A_Private.BP.CPMissingRequirement")
#endif
