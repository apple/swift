target_sources(swiftSILOptimizer PRIVATE
  BorrowScopeOpts.cpp
  Context.cpp
  CopyValueOpts.cpp
  LoadCopyToLoadBorrowOpt.cpp
  MoveValueOpts.cpp
  OwnedToGuaranteedPhiOpt.cpp
  OwnershipConversionElimination.cpp
  OwnershipLiveRange.cpp
  SemanticARCOptVisitor.cpp
  SemanticARCOpts.cpp
  )
