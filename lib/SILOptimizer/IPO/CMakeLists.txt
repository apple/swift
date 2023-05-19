target_sources(swiftSILOptimizer PRIVATE
  CapturePropagation.cpp
  ClosureSpecializer.cpp
  CrossModuleOptimization.cpp
  DeadFunctionElimination.cpp
  GlobalPropertyOpt.cpp
  LetPropertiesOpts.cpp
  UsePrespecialized.cpp)
