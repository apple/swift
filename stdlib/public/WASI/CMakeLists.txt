add_swift_target_library(swiftWasiPthread STATIC IS_STDLIB
  Pthread.cpp
  TARGET_SDKS WASI
  INSTALL_IN_COMPONENT stdlib)
