# This source file is part of the Swift.org open source project
#
# Copyright (c) 2014 - 2021 Apple Inc. and the Swift project authors
# Licensed under Apache License v2.0 with Runtime Library Exception
#
# See http://swift.org/LICENSE.txt for license information
# See http://swift.org/CONTRIBUTORS.txt for Swift project authors

swift_compiler_sources(Optimizer
  SimplifyApply.swift
  SimplifyBeginCOWMutation.swift
  SimplifyBranch.swift
  SimplifyBuiltin.swift
  SimplifyCondBranch.swift
  SimplifyCondFail.swift
  SimplifyDebugStep.swift
  SimplifyDestructure.swift
  SimplifyGlobalValue.swift
  SimplifyLoad.swift
  SimplifyPartialApply.swift
  SimplifyStrongRetainRelease.swift
  SimplifyStructExtract.swift
  SimplifyTupleExtract.swift
  SimplifyUncheckedEnumData.swift)
