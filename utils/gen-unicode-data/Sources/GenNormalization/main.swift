//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2021 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

import GenUtils

// Main entry point into the normalization generator.
func generateNormalization(for platform: String, version: String) {
  var result = readFile("Input/NormalizationData.h")
  
  let derivedNormalizationProps = readFile("Data/\(version)/DerivedNormalizationProps.txt")

  let unicodeData: String
  
  switch platform {
  case "Apple":
    unicodeData = readFile("Data/\(version)/Apple/UnicodeData.txt")
  default:
    unicodeData = readFile("Data/\(version)/UnicodeData.txt")
  }
  
  // Get all NFX_QC information and put it together with CCC info.
  var normData: [UInt32: UInt16] = [:]
  getQCData(from: derivedNormalizationProps, with: &normData)
  getCCCData(from: unicodeData, with: &normData)
  
  // Take the NFX_QC info and CCC and emit it as a singular "normData".
  let flattenedNormData = flatten(Array(normData))
  emitNormData(flattenedNormData, into: &result)
  
  // Get and emit decomposition data.
  let decompData = getDecompData(from: unicodeData)
  let decompMph = mph(for: decompData.map { UInt64($0.0) })
  emitDecomp(decompMph, decompData, into: &result)
  
  // Get and emit composition data. (Remove composition exclusions)
  let compExclusions = getCompExclusions(from: derivedNormalizationProps)
  let filteredDecomp = decompData.filter { (scalar, _) in
    !compExclusions.contains {
      $0.contains(scalar)
    }
  }
  let compMph = mph(for: Array(Set(filteredDecomp.map { UInt64($0.1[1]) })))
  emitComp(compMph, filteredDecomp, into: &result)
  
  result += """
  #endif // #ifndef NORMALIZATION_DATA_H
  
  """
  
  // Finally, write it out.
  write(result, to: "Output/\(platform)/\(version)/NormalizationData.h")
}

for platform in ["Common", "Apple"] {
  generateNormalization(for: platform, version: "15")
}
