target_sources(swiftSILOptimizer PRIVATE
  SemanticARCOpts.cpp
  OwnershipLiveRange.cpp
  LoadCopyToLoadBorrowOpt.cpp
  BorrowScopeOpts.cpp
  CopyValueOpts.cpp
  OwnedToGuaranteedPhiOpt.cpp
  Context.cpp
  SemanticARCOptVisitor.cpp
  CoroutineLifetimeExtender.cpp
  )
