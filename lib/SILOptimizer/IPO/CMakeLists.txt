target_sources(swiftSILOptimizer PRIVATE
  CapturePropagation.cpp
  ClosureSpecializer.cpp
  CrossDeadFunctionElimination.cpp
  CrossModuleSerializationSetup.cpp
  DeadFunctionElimination.cpp
  GlobalOpt.cpp
  GlobalPropertyOpt.cpp
  LetPropertiesOpts.cpp
  UsePrespecialized.cpp)
