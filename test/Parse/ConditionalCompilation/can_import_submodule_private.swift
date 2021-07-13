// RUN: %target-typecheck-verify-swift -I %S/Inputs/can-import-submodule/ 

#if !canImport(A.Private)
#error("should can import A.Private")
#endif

#if canImport(A_Private)
import A_Private
#else
#error("should can import A_Private")
#endif

#if canImport(A_Private.BP)
import A_Private.BP
#else
#error("should can import A_Private.BP")
#endif

#if canImport(A_Private.BP.CP)
import A_Private.BP.CP
#else
#error("should can import A_Private.BP.CP")
#endif


#if canImport(A_Private.Z)
#error("should not can import A_Private.Z")
#endif

#if canImport(A_Private.BP.Z)
#error("should not can import A_Private.BP.Z")
#endif

#if canImport(A_Private.Z.CP)
#error("should not can import A_Private.Z.CP")
#endif

#if canImport(A_Private.BP.CP.Z)
#error("should not can import A_Private.BP.CP.Z")
#endif
