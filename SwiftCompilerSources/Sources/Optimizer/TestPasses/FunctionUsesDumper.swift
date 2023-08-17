//===--- FunctionUsesDumper.swift -----------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2022 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

import SIL

let functionUsesDumper = ModulePass(name: "dump-function-uses") {
    (context: ModulePassContext) in

  var functionUses = FunctionUses()
  functionUses.collect(context: context)

  for function in context.functions {
    let uses = functionUses.getUses(of: function)
    
    print("Uses of \(function.name)")
    print(uses)
    print("End function \(function.name)\n")
  }
}
