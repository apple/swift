#===--- CMakeLists.txt - Concurrency support library ---------------------===#
#
# This source file is part of the Swift.org open source project
#
# Copyright (c) 2019 - 2022 Apple Inc. and the Swift project authors
# Licensed under Apache License v2.0 with Runtime Library Exception
#
# See https://swift.org/LICENSE.txt for license information
# See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
#
#===----------------------------------------------------------------------===#

if(SWIFT_BUILD_SDK_OVERLAY)
set(swift_distributed_darwin_depencencies Darwin)
else()
set(swift_distributed_darwin_depencencies)
endif()

set(swift_distributed_link_libraries
  swiftCore)


add_swift_target_library(swiftDistributed ${SWIFT_STDLIB_LIBRARY_BUILD_TYPES} IS_STDLIB
  DistributedActor.cpp
  DistributedActor.swift
  DistributedActorSystem.swift
  DistributedAssertions.swift
  DistributedDefaultExecutor.swift
  DistributedMacros.swift
  DistributedMetadata.swift
  LocalTestingDistributedActorSystem.swift

  SWIFT_MODULE_DEPENDS_IOS ${swift_distributed_darwin_depencencies}
  SWIFT_MODULE_DEPENDS_OSX ${swift_distributed_darwin_depencencies}
  SWIFT_MODULE_DEPENDS_TVOS ${swift_distributed_darwin_depencencies}
  SWIFT_MODULE_DEPENDS_WATCHOS ${swift_distributed_darwin_depencencies}
  SWIFT_MODULE_DEPENDS_ANDROID Android
  SWIFT_MODULE_DEPENDS_LINUX Glibc
  SWIFT_MODULE_DEPENDS_FREEBSD Glibc
  SWIFT_MODULE_DEPENDS_OPENBSD Glibc
  SWIFT_MODULE_DEPENDS_CYGWIN Glibc
  SWIFT_MODULE_DEPENDS_HAIKU Glibc
  SWIFT_MODULE_DEPENDS_WINDOWS CRT WinSDK

  LINK_LIBRARIES ${swift_distributed_link_libraries}

  C_COMPILE_FLAGS
    -DswiftDistributed_EXPORTS
    -I${SWIFT_SOURCE_DIR}/stdlib/include

  SWIFT_COMPILE_FLAGS
    ${SWIFT_STANDARD_LIBRARY_SWIFT_FLAGS}
    -parse-stdlib

  LINK_FLAGS
    "${SWIFT_RUNTIME_SWIFT_LINK_FLAGS}"

  SWIFT_MODULE_DEPENDS _Concurrency
  INSTALL_IN_COMPONENT stdlib
)
