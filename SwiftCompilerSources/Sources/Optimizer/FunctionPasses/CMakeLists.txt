# This source file is part of the Swift.org open source project
#
# Copyright (c) 2014 - 2021 Apple Inc. and the Swift project authors
# Licensed under Apache License v2.0 with Runtime Library Exception
#
# See http://swift.org/LICENSE.txt for license information
# See http://swift.org/CONTRIBUTORS.txt for Swift project authors

swift_compiler_sources(Optimizer
  AssumeSingleThreaded.swift
  CleanupDebugSteps.swift
  ComputeEscapeEffects.swift
  ComputeSideEffects.swift
  InitializeStaticGlobals.swift
  ObjectOutliner.swift
  ObjCBridgingOptimization.swift
  MergeCondFails.swift
  NamedReturnValueOptimization.swift
  ReleaseDevirtualizer.swift
  SimplificationPasses.swift
  StackPromotion.swift
  StripObjectHeaders.swift
)
