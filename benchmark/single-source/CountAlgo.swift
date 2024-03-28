//===--- CountAlgo.swift --------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2018 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
import TestsUtils

public let benchmarks = [
  BenchmarkInfo(
    name: "CountAlgoArray",
    runFunction: run_CountAlgoArray,
    tags: [.validation, .api]),
  BenchmarkInfo(
    name: "CountAlgoString",
    runFunction: run_CountAlgoString,
    tags: [.validation, .api],
    legacyFactor: 5),
]

@inline(never)
public func run_CountAlgoArray(_ N: Int) {
  for _ in 1...10*N {
    CheckResults(numbers.count(where: { $0 & 4095 == 0 }) == 25)
  }
}

@inline(never)
public func run_CountAlgoString(_ N: Int) {
  let vowels = Set("aeiou")
  for _ in 1...N {
    CheckResults(text.count(where: vowels.contains) == 2014)
  }
}

let numbers = Array(0..<100_000)

let text = """
    Lorem ipsum dolor sit amet, consectetur adipiscing elit. Maecenas tempus
    dictum tellus placerat ultrices. Proin mauris risus, eleifend a elit ut,
    semper consectetur nibh. Nulla ultricies est a vehicula rhoncus. Morbi
    sollicitudin efficitur est a hendrerit. Interdum et malesuada fames ac ante
    ipsum primis in faucibus. Lorem ipsum dolor sit amet, consectetur
    adipiscing elit. Nulla facilisi. Sed euismod sagittis laoreet. Ut elementum
    tempus ultrices. Donec convallis mauris at faucibus maximus.
    Nullam in nunc sit amet ante tristique elementum quis ut eros. Fusce
    dignissim, ante at efficitur dapibus, ex massa convallis nibh, et venenatis
    leo leo sit amet nisl. Lorem ipsum dolor sit amet, consectetur adipiscing
    elit. Quisque sed mi eu mi rutrum accumsan vel non massa. Nunc condimentum,
    arcu eget interdum hendrerit, ipsum mi pretium felis, ut mollis erat metus
    non est. Donec eu sapien id urna lobortis eleifend et eu ipsum. Mauris
    purus dolor, consequat ac nulla a, vehicula sollicitudin nulla.
    Phasellus a congue diam. Curabitur sed orci at sem laoreet facilisis eget
    quis est. Pellentesque habitant morbi tristique senectus et netus et
    malesuada fames ac turpis egestas. Maecenas justo tellus, efficitur id
    velit at, mollis pellentesque mi. Vivamus maximus nibh et ipsum porttitor
    facilisis. Curabitur cursus lobortis erat. Sed vitae eros et dolor feugiat
    consequat. In ac massa in odio gravida dignissim. Praesent aliquam gravida
    ullamcorper.
    Etiam feugiat sit amet odio sed tincidunt. Duis dolor odio, posuere at
    pretium sed, dignissim eu diam. Aenean eu convallis orci, vitae finibus
    erat. Aliquam nec mollis tellus. Morbi luctus sed quam et vestibulum.
    Praesent id diam tempus, consectetur tortor vel, auctor orci. Aliquam
    congue ex eu sagittis sodales. Suspendisse non convallis nulla. Praesent
    elementum semper augue, et fringilla risus ullamcorper id. Fusce eu lorem
    sit amet augue fermentum tincidunt. In aliquam libero sit amet dui rhoncus,
    ac scelerisque sem porttitor. Cras venenatis, nisi quis ullamcorper
    dapibus, odio dolor rutrum magna, vel pellentesque sem lectus in tellus.
    Proin faucibus leo iaculis nulla egestas molestie.
    Phasellus vitae tortor vitae erat elementum feugiat vel vel enim. Phasellus
    fringilla lacus sed venenatis dapibus. Phasellus sagittis vel neque ut
    varius. Proin aliquam, lectus sit amet auctor finibus, lorem libero
    pellentesque turpis, ac condimentum augue felis sit amet sem. Pellentesque
    pharetra nisl nec est congue, in posuere felis maximus. In ut nulla
    sodales, pharetra neque et, venenatis dui. Mauris imperdiet, arcu vel
    hendrerit vehicula, elit massa consectetur purus, eu blandit nunc orci sit
    amet turpis. Vestibulum ultricies id lorem id maximus. Pellentesque
    feugiat, lacus et aliquet consequat, mi leo vehicula justo, dapibus dictum
    mi quam convallis magna. Quisque id pulvinar dui, consequat gravida nisl.
    Nam nec justo venenatis, tincidunt enim a, iaculis odio. Maecenas eget
    lorem posuere, euismod nisl vel, pulvinar ex. Maecenas vitae risus ipsum.
    Proin congue sem ante, sit amet sagittis odio mattis sit amet. Nullam et
    nisi nulla.
    Donec vel hendrerit metus. Praesent quis finibus erat. Aliquam erat
    volutpat. Fusce sit amet ultricies tellus, vitae dictum dolor. Morbi auctor
    dolor vel ligula pretium aliquam. Aenean lobortis vel magna vel ultricies.
    Aenean porta urna vitae ornare porta. Quisque pretium dui diam, quis
    iaculis odio venenatis non. Maecenas at lacus et ligula tincidunt feugiat
    eu vel ipsum. Proin fermentum elit et quam tempus, eget pulvinar nisl
    pharetra.
    Mauris sodales tempus erat in lobortis. Duis vitae lacinia sapien.
    Pellentesque vitae massa eget orci sodales aliquet. Orci varius natoque
    penatibus et magnis dis parturient montes, nascetur ridiculus mus. Fusce
    nisi arcu, egestas vel consectetur eu, auctor et metus. In ultricies ligula
    felis, vitae pellentesque dolor tempor ac. Praesent mi magna, ultrices ut
    ultrices vel, sollicitudin a leo.
    Nam porta, nisi in scelerisque consequat, leo lacus accumsan massa,
    venenatis faucibus tellus quam eget tellus. Curabitur pulvinar, tellus ac
    facilisis consectetur, lacus lacus venenatis est, eu pretium orci augue
    gravida nunc. Aenean odio tellus, facilisis et finibus id, varius vitae
    diam. Aenean at suscipit sem. Suspendisse porta neque at nibh semper, sit
    amet suscipit libero egestas. Donec commodo vitae justo vitae laoreet.
    Suspendisse dignissim erat id ante maximus porta. Curabitur hendrerit
    maximus odio, et maximus felis malesuada eu. Integer dapibus finibus diam,
    quis convallis metus bibendum non.
    In vel vulputate nisi, non lacinia nunc. Nullam vitae ligula finibus,
    varius arcu in, pellentesque ipsum. Morbi vel velit tincidunt quam cursus
    lacinia non in neque. Suspendisse id feugiat nibh. Vestibulum egestas eu
    leo viverra fringilla. Curabitur ultrices sollicitudin libero, non sagittis
    felis consectetur id. Aenean non metus eget leo ornare porta sed in metus.
    Nullam quis fermentum sapien, sit amet sodales mi. Maecenas nec purus urna.
    Phasellus condimentum enim nec magna convallis, eu lacinia libero
    scelerisque. Suspendisse justo libero, maximus in auctor id, euismod quis
    risus. Nam eget augue diam. Ut id risus pulvinar elit consectetur varius.
    Aliquam tincidunt tortor pretium feugiat tempor. Nunc nec feugiat ex.
    Ut pulvinar augue eget pharetra vehicula. Phasellus malesuada tempor sem,
    ut tincidunt velit convallis in. Vivamus luctus libero vitae massa tempus,
    id elementum urna iaculis. Sed eleifend quis purus quis convallis. In
    rhoncus interdum mollis. Pellentesque dictum euismod felis, eget lacinia
    elit blandit vel. Praesent elit velit, pharetra a sodales in, cursus vitae
    tortor. In vitae scelerisque tellus.
    """
