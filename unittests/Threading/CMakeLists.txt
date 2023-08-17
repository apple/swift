if(("${SWIFT_HOST_VARIANT_SDK}" STREQUAL "${SWIFT_PRIMARY_VARIANT_SDK}") AND
   ("${SWIFT_HOST_VARIANT_ARCH}" STREQUAL "${SWIFT_PRIMARY_VARIANT_ARCH}"))
  add_swift_unittest(SwiftThreadingTests
    Mutex.cpp
    ConditionVariable.cpp
    Once.cpp
    LinuxUlock.cpp
    Fatal.cpp
    )

  target_link_libraries(SwiftThreadingTests
    PRIVATE
    swiftThreading${SWIFT_PRIMARY_VARIANT_SUFFIX}
    swiftCore${SWIFT_PRIMARY_VARIANT_SUFFIX}
    )

  if(SWIFT_HOST_VARIANT STREQUAL "windows")
    target_link_libraries(SwiftThreadingTests PRIVATE Synchronization)
  endif()
endif()
