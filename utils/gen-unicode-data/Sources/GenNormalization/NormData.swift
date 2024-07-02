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

func emitNormData(
  _ data: [(ClosedRange<UInt32>, UInt16)],
  into result: inout String
) {
  let uniqueData = Set(data.map { $0.1 }).sorted()

  // 64 bit arrays * 8 bytes = .512 KB
  var bitArrays: [BitArray] = .init(repeating: .init(size: 64), count: 64)
  
  let chunkSize = 0x110000 / 64 / 64
  
  var chunks: [Int] = []
  
  for i in 0 ..< 64 * 64 {
    let lower = i * chunkSize
    let upper = lower + chunkSize - 1
    
    let idx = i / 64
    let bit = i % 64
    
    for scalar in lower ... upper {
      if data.contains(where: { $0.0.contains(UInt32(scalar)) }) {
        chunks.append(i)

        bitArrays[idx][bit] = true
        break
      }
    }
  }
  
  // Remove the trailing 0s. Currently this reduces quick look size down to
  // 96 bytes from 512 bytes.
  var reducedBA = Array(bitArrays.reversed())
  reducedBA = Array(reducedBA.drop {
    $0.words == [0x0]
  })
  
  bitArrays = reducedBA.reversed()
  
  // Keep a record of every rank for all the bitarrays.
  var ranks: [UInt16] = []
  
  // Record our quick look ranks.
  var lastRank: UInt16 = 0
  for (i, _) in bitArrays.enumerated() {
    guard i != 0 else {
      ranks.append(0)
      continue
    }
    
    var rank = UInt16(bitArrays[i - 1].words[0].nonzeroBitCount)
    rank += lastRank
    
    ranks.append(rank)
    
    lastRank = rank
  }
  
  // Insert our quick look size at the beginning.
  var size = BitArray(size: 64)
  size.words = [UInt64(bitArrays.count)]
  bitArrays.insert(size, at: 0)
  
  var dataIndices: [UInt8] = []
  
  for chunk in chunks {
    var chunkBA = BitArray(size: chunkSize)
    
    let lower = chunk * chunkSize
    let upper = lower + chunkSize
    
    let chunkDataIdx = UInt64(dataIndices.endIndex)
    
    // Insert our chunk's data index in the upper bits of the last word of our
    // bit array.
    chunkBA.words[chunkBA.words.endIndex - 1] |= chunkDataIdx << 16
    
    for scalar in lower ..< upper {
      if data.contains(where: { $0.0.contains(UInt32(scalar)) }) {
        chunkBA[scalar % chunkSize] = true
        
        let data = data[data.firstIndex {
          $0.0.contains(UInt32(scalar))
        }!].1
        
        let dataIdx = uniqueData.firstIndex(of: data)!
        
        dataIndices.append(UInt8(dataIdx))
      }
    }
    
    // Append our chunk bit array's rank.
    var lastRank: UInt16 = 0
    for (i, _) in chunkBA.words.enumerated() {
      guard i != 0 else {
        ranks.append(0)
        continue
      }
      
      var rank = UInt16(chunkBA.words[i - 1].nonzeroBitCount)
      rank += lastRank
      
      ranks.append(rank)
      lastRank = rank
    }
    
    bitArrays += chunkBA.words.map {
      var ba = BitArray(size: 64)
      ba.words = [$0]
      return ba
    }
  }
  
  emitCollection(
    uniqueData,
    name: "_swift_stdlib_normData_data",
    into: &result
  )
  
  emitCollection(
    dataIndices,
    name: "_swift_stdlib_normData_data_indices",
    into: &result
  )
  
  emitCollection(
    ranks,
    name: "_swift_stdlib_normData_ranks",
    into: &result
  )
  
  emitCollection(
    bitArrays,
    name: "_swift_stdlib_normData",
    type: "__swift_uint64_t",
    into: &result
  ) {
    "0x\(String($0.words[0], radix: 16, uppercase: true))"
  }
}
