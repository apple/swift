//===--- StringTests.swift ------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2021 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
import TestsUtils

@_spi(_Unicode)
import Swift

public var benchmarks: [BenchmarkInfo] {
  var result = [
    BenchmarkInfo(
      name: "StringEqualPointerComparison",
      runFunction: run_StringEqualPointerComparison,
      tags: [.validation, .api, .String]),
    BenchmarkInfo(
      name: "StringHasPrefixAscii",
      runFunction: run_StringHasPrefixAscii,
      tags: [.validation, .api, .String],
      legacyFactor: 10),
    BenchmarkInfo(
      name: "StringHasPrefixUnicode",
      runFunction: run_StringHasPrefixUnicode,
      tags: [.validation, .api, .String],
      legacyFactor: 1000),
    BenchmarkInfo(
      name: "StringHasSuffixAscii",
      runFunction: run_StringHasSuffixAscii,
      tags: [.validation, .api, .String],
      legacyFactor: 10),
    BenchmarkInfo(
      name: "StringHasSuffixUnicode",
      runFunction: run_StringHasSuffixUnicode,
      tags: [.validation, .api, .String],
      legacyFactor: 1000),
  ]

  if #available(macOS 15.0, iOS 18.0, watchOS 11.0, tvOS 18.0, visionOS 2.0, *) {
    result.append(
      BenchmarkInfo(
        name: "StringIterateWords",
        runFunction: run_iterateWords,
        tags: [.validation, .String]))
  }
  return result
}

// FIXME(string)
public func run_StringHasPrefixAscii(_ n: Int) {
#if _runtime(_ObjC)
  let prefix = "prefix"
  let testString = "prefixedString"
  for _ in 0 ..< n {
    for _ in 0 ..< 10_000 {
      check(testString.hasPrefix(getString(prefix)))
    }
  }
#endif
}

// FIXME(string)
public func run_StringHasSuffixAscii(_ n: Int) {
#if _runtime(_ObjC)
  let suffix = "Suffixed"
  let testString = "StringSuffixed"
  for _ in 0 ..< n {
    for _ in 0 ..< 10_000 {
      check(testString.hasSuffix(getString(suffix)))
    }
  }
#endif
}

// FIXME(string)
public func run_StringHasPrefixUnicode(_ n: Int) {
#if _runtime(_ObjC)
  let prefix = "❄️prefix"
  let testString = "❄️prefixedString"
  for _ in 0 ..< n {
    for _ in 0 ..< 100 {
      check(testString.hasPrefix(getString(prefix)))
    }
  }
#endif
}

// FIXME(string)
public func run_StringHasSuffixUnicode(_ n: Int) {
#if _runtime(_ObjC)
  let suffix = "❄️Suffixed"
  let testString = "String❄️Suffixed"
  for _ in 0 ..< n {
    for _ in 0 ..< 100 {
      check(testString.hasSuffix(getString(suffix)))
    }
  }
#endif
}

@inline(never)
internal func compareEqual(_ str1: String, _ str2: String) -> Bool {
  return str1 == str2
}

public func run_StringEqualPointerComparison(_ n: Int) {
  let str1 = "The quick brown fox jumps over the lazy dog. The quick brown fox jumps over the lazy dog. The quick brown fox jumps over the lazy dog. The quick brown fox jumps over the lazy dog. The quick brown fox jumps over the lazy dog. The quick brown fox jumps over the lazy dog. The quick brown fox jumps over the lazy dog. The quick brown fox jumps over the lazy dog. The quick brown fox jumps over the lazy dog. The quick brown fox jumps over the lazy dog. The quick brown fox jumps over the lazy dog. The quick brown fox jumps over the lazy dog. The quick brown fox jumps over the lazy dog. The quick brown fox jumps over the lazy dog. The quick brown fox jumps over the lazy dog. The quick brown fox jumps over the lazy dog. The quick brown fox jumps over the lazy dog. The quick brown fox jumps over the lazy dog. The quick brown fox jumps over the lazy dog. The quick brown fox jumps over the lazy dog. The quick brown fox jumps over the lazy dog. The quick brown fox jumps over the lazy dog. The quick brown fox jumps over the lazy dog. The quick brown fox jumps over the lazy dog. The quick brown fox jumps over the lazy dog. The quick brown fox jumps over the lazy dog. The quick brown fox jumps over the lazy dog. The quick brown fox jumps over the lazy dog. The quick brown fox jumps over the lazy dog. The quick brown fox jumps over the lazy dog. The quick brown fox jumps over the lazy dog. The quick brown fox jumps over the lazy dog. The quick brown fox jumps over the lazy dog. The quick brown fox jumps over the lazy dog. The quick brown fox jumps over the lazy dog. The quick brown fox jumps over the lazy dog. The quick brown fox jumps over the lazy dog. The quick brown fox jumps over the lazy dog. The quick brown fox jumps over the lazy dog. "
  let str2 = str1
  for _ in 0 ..< n {
    for _ in 0 ..< 100_000 {
      check(compareEqual(str1, str2))
    }
  }
}

let swiftOrgHTML = """
<!DOCTYPE html>
<html lang="en">

<head>
  <meta charset="utf-8" />
  <title>Swift.org - Blog</title>
  <meta name="author" content="Apple Inc." />
  <meta name="viewport" content="width=device-width initial-scale=1" />
  <link rel="license" href="/LICENSE.txt" />
  <link rel="stylesheet" media="all" href="/assets/stylesheets/application.css" />
  <link rel="shortcut icon" sizes="16x16 24x24 32x32 48x48 64x64" type="image/vnd.microsoft.icon" href="/favicon.ico" />
  <link rel="apple-touch-icon" href="/apple-touch-icon.png" />
  <link rel="apple-touch-icon" sizes="57x57" href="/apple-touch-icon-57x57.png" />
  <link rel="apple-touch-icon" sizes="72x72" href="/apple-touch-icon-72x72.png" />
  <link rel="apple-touch-icon" sizes="76x76" href="/apple-touch-icon-76x76.png" />
  <link rel="apple-touch-icon" sizes="114x114" href="/apple-touch-icon-114x114.png" />
  <link rel="apple-touch-icon" sizes="120x120" href="/apple-touch-icon-120x120.png" />
  <link rel="apple-touch-icon" sizes="144x144" href="/apple-touch-icon-144x144.png" />
  <link rel="apple-touch-icon" sizes="152x152" href="/apple-touch-icon-152x152.png" />
  <link rel="apple-touch-icon" sizes="180x180" href="/apple-touch-icon-180x180.png" />
  <link rel="mask-icon" href="/assets/images/icon-swift.svg" color="#F05339" />
  
  <link rel="alternate" type="application/atom+xml" title="Swift.org (Atom Feed)" href="/atom.xml" />
  

  
  <link rel="canonical" href="https://swift.org/blog/" />
  

  <meta name="twitter:card" content="summary" />
  <meta name="twitter:site" content="@SwiftLang" />
  
  <meta name="twitter:title" content="Swift.org" />
  <meta name="twitter:description" content="Swift is a general-purpose programming language built using a modern approach to safety, performance, and software design patterns." />
  

  <meta property="og:site_name" content="Swift.org" />
  <meta property="og:image" content="https://swift.org/apple-touch-icon-180x180.png" />
  
  
  <meta property="og:title" content="Swift.org" />
  <meta property="og:url" content="https://swift.org" />
  <meta property="og:description" content="Swift is a general-purpose programming language built using a modern approach to safety, performance, and software design patterns." />
  
</head>

<body>
<script src="/assets/javascripts/color-scheme-toggle.js"></script>
<nav role="navigation">
  <div class="nav-menu-container">
    <header class="menu-item logo-container" role="banner">
      <h1 id="logo">
        <a href="/" title="Swift.org" role="img" aria-label="Swift.org">
          Swift.org
        </a>
      </h1>
    </header>
    <div id="menu-toggle" class="menu-item menu-toggle open"></div>
  </div>

  <div class="list-items">
    
    
    <ul>
      
      
      <li>
      
        <a href="/about/">About Swift</a>
        
      </li>
      
      
      <li class="active">
      
        <a href="/blog/">Blog</a>
        
      </li>
      
      
      <li>
      
        <a href="/getting-started/">Getting Started</a>
        
      </li>
      
      
      <li>
      
        <a href="/download/">Download</a>
        
      </li>
      
      
      <li>
      
        <a href="/platform-support/">Platform Support</a>
        
      </li>
      
      
      <li>
      
        <a href="/documentation/">Documentation</a>
        
      </li>
      
    </ul>
    
    
    <h2>Community</h2>
    
    <ul>
      
      
      <li>
      
        <a href="/community/">Community Overview</a>
        
      </li>
      
      
      <li>
      
        <a href="/diversity/">Diversity</a>
        
      </li>
      
      
      <li>
      
        <a href="/mentorship/">Mentorship</a>
        
      </li>
      
      
      <li>
      
        <a href="/contributing/">Contributing</a>
        
      </li>
      
      
      <li>
      
        <a href="/code-of-conduct/">Code of Conduct</a>
        
      </li>
      
    </ul>
    
    
    <h2>Open Source Development</h2>
    
    <ul>
      
      
      <li>
      
        <a href="/source-code/">Source Code</a>
        
      </li>
      
      
      <li>
      
        <a href="/continuous-integration/">Continuous Integration</a>
        
      </li>
      
      
      <li>
      
        <a href="/source-compatibility/">Source Compatibility</a>
        
      </li>
      
      
      <li>
      
        <a href="/support/security.html">Security</a>
        
      </li>
      
    </ul>
    
    
    <h2>Open Source Efforts</h2>
    
    <ul>
      
      
      <li>
      
        <a href="/swift-compiler/">Swift Compiler</a>
        
      </li>
      
      
      <li>
      
        <a href="/standard-library/">Standard Library</a>
        
      </li>
      
      
      <li>
      
        <a href="/package-manager/">Package Manager</a>
        
      </li>
      
      
      <li>
      
        <a href="/core-libraries/">Core Libraries</a>
        
      </li>
      
      
      <li>
      
        <a href="/lldb/">REPL, Debugger & Playgrounds</a>
        
      </li>
      
      
      <li>
      
        <a href="/server/">Swift on Server</a>
        
      </li>
      
      
      <li>
      
        <a href="/website/">Swift.org website</a>
        
      </li>
      
    </ul>
    
  </div>
</nav>

<main role="main">
  
  <article id="/blog/language-workgroup" class="summary">
    <header>
      <h1 class="title"><a href="/blog/language-workgroup/">Announcing the Language Workgroup</a></h1>
      <time pubdate datetime="2022-06-15T05:20:00-04:00">June 15, 2022</time>
    </header>
    <section class="excerpt">
      <p>The Swift community has accomplished a great deal together, with hundreds of changes to Swift through the Swift Evolution process and significant advances to the language and tooling since Swift became an open-source project. In recent years, there has been increased momentum in the community through various workgroups, including Diversity in Swift and the Server Workgroup.  The Core Team recognizes the opportunity to tap into the potential of these workgroups to amplify the impact of the community and support more members of the community driving impactful investments.</p>


      <a href="/blog/language-workgroup/">Read more...</a>
    </section>
  </article>

  <article id="/blog/mentorship-2022" class="summary">
    <header>
      <h1 class="title"><a href="/blog/mentorship-2022/">Celebrating learning experiences from the 2021 Swift Mentorship Program</a></h1>
      <time pubdate datetime="2022-05-19T06:00:00-04:00">May 19, 2022</time>
    </header>
    <section class="excerpt">
      <p>As we prepare for the 2022 Swift Mentorship Program, we’re excited to share insights from a few of last year’s mentees on their learning journey.</p>


      <a href="/blog/mentorship-2022/">Read more...</a>
    </section>
  </article>

  <article id="/blog/sswg-update" class="summary">
    <header>
      <h1 class="title"><a href="/blog/sswg-update/">SSWG 2021 Annual Update</a></h1>
      <time pubdate datetime="2022-04-25T07:00:00-04:00">April 25, 2022</time>
    </header>
    <section class="excerpt">
      <p>Since the last update from the SSWG, the Swift on Server ecosystem has continued to grow and expand.</p>


      <a href="/blog/sswg-update/">Read more...</a>
    </section>
  </article>

  <article id="/blog/swift-async-algorithms" class="summary">
    <header>
      <h1 class="title"><a href="/blog/swift-async-algorithms/">Introducing Swift Async Algorithms</a></h1>
      <time pubdate datetime="2022-03-24T07:00:00-04:00">March 24, 2022</time>
    </header>
    <section class="excerpt">
      <p>As part of Swift’s move toward safe, simple, and performant asynchronous programming, we are pleased to introduce a new package of algorithms for <code class="language-plaintext highlighter-rouge">AsyncSequence</code>. It is called <strong>Swift Async Algorithms</strong> and it is available now <a href="https://github.com/apple/swift-async-algorithms">on GitHub</a>.</p>


      <a href="/blog/swift-async-algorithms/">Read more...</a>
    </section>
  </article>

  <article id="/blog/website-open-source" class="summary">
    <header>
      <h1 class="title"><a href="/blog/website-open-source/">Swift.org Website is Now Open Source</a></h1>
      <time pubdate datetime="2022-03-15T05:00:00-04:00">March 15, 2022</time>
    </header>
    <section class="excerpt">
      <p>The Swift.org site has long served as the hub where developers come together to work on the open source Swift compiler, libraries, and tools.
Today, we are happy to announce that the Swift.org website itself is also an open source project, ready for community contributions.
With this move, the website is also expanding its mandate to better support the entire community of Swift users, not just contributors.</p>


      <a href="/blog/website-open-source/">Read more...</a>
    </section>
  </article>

  <article id="/blog/swift-5.6-released" class="summary">
    <header>
      <h1 class="title"><a href="/blog/swift-5.6-released/">Swift 5.6 Released!</a></h1>
      <time pubdate datetime="2022-03-14T06:00:00-04:00">March 14, 2022</time>
    </header>
    <section class="excerpt">
      <p>Swift 5.6 is now officially released!</p>


      <a href="/blog/swift-5.6-released/">Read more...</a>
    </section>
  </article>

  <article id="/blog/distributed-actors" class="summary">
    <header>
      <h1 class="title"><a href="/blog/distributed-actors/">Introducing Swift Distributed Actors</a></h1>
      <time pubdate datetime="2021-10-28T05:00:00-04:00">October 28, 2021</time>
    </header>
    <section class="excerpt">
      <p>We’re thrilled to announce a new open-source package for the Swift on Server ecosystem, <a href="https://github.com/apple/swift-distributed-actors/">Swift Distributed Actors</a>, a complete server-oriented cluster library for the upcoming <code class="language-plaintext highlighter-rouge">distributed actor</code> language feature!</p>


      <a href="/blog/distributed-actors/">Read more...</a>
    </section>
  </article>

  <article id="/blog/swift-docc" class="summary">
    <header>
      <h1 class="title"><a href="/blog/swift-docc/">Swift-DocC is Now Open Source</a></h1>
      <time pubdate datetime="2021-10-13T06:00:00-04:00">October 13, 2021</time>
    </header>
    <section class="excerpt">
      <p>At WWDC21, Apple announced Swift-DocC, a new documentation compiler for Swift frameworks and
packages. Swift-DocC provides an effortless way to author great documentation alongside your code,
and generate comprehensive documentation websites for Swift codebases. It supports API docs authored
as code comments, long-form conceptual articles written in Markdown, and even step-by-step tutorials
with integrated images.</p>


      <a href="/blog/swift-docc/">Read more...</a>
    </section>
  </article>

  <article id="/blog/swift-5.5-released" class="summary">
    <header>
      <h1 class="title"><a href="/blog/swift-5.5-released/">Swift 5.5 Released!</a></h1>
      <time pubdate datetime="2021-09-20T06:00:00-04:00">September 20, 2021</time>
    </header>
    <section class="excerpt">
      <p>Swift 5.5 is now officially released!  Swift 5.5 is a massive release, which includes newly introduced language capabilities for concurrency, including <code class="language-plaintext highlighter-rouge">async/await</code>, structured concurrency, and Actors.  My heartfelt thanks to the entire Swift community for all the active discussion, review, and iteration on the concurrency (and other additions) that make up the release.  Thank you!</p>


      <a href="/blog/swift-5.5-released/">Read more...</a>
    </section>
  </article>

  <article id="/blog/package-collections" class="summary">
    <header>
      <h1 class="title"><a href="/blog/package-collections/">Package Collections</a></h1>
      <time pubdate datetime="2021-06-07T05:00:00-04:00">June 7, 2021</time>
    </header>
    <section class="excerpt">
      <p>In Swift 5.5, the Swift Package Manager adds support for package collections — bite size curated lists of packages that make it easy to discover, share and adopt packages.</p>


      <a href="/blog/package-collections/">Read more...</a>
    </section>
  </article>

  <article id="/blog/swift-mentorship-program" class="summary">
    <header>
      <h1 class="title"><a href="/blog/swift-mentorship-program/">Announcing the Swift Mentorship Program</a></h1>
      <time pubdate datetime="2021-05-10T06:00:00-04:00">May 10, 2021</time>
    </header>
    <section class="excerpt">
      <p>We’re thrilled to announce the Swift Mentorship Program — a new contributor program for the Swift community and part of the <a href="/diversity">Diversity in Swift</a> initiative. The Swift Mentorship Program is designed to support developers as they become active open source contributors to the Swift project, providing direct mentorship with experienced members of the community.</p>


      <a href="/blog/swift-mentorship-program/">Read more...</a>
    </section>
  </article>

  <article id="/blog/swift-5.4-released" class="summary">
    <header>
      <h1 class="title"><a href="/blog/swift-5.4-released/">Swift 5.4 Released!</a></h1>
      <time pubdate datetime="2021-04-26T06:00:00-04:00">April 26, 2021</time>
    </header>
    <section class="excerpt">
      <p>Swift 5.4 is now officially released!  This release contains a variety of language and tooling improvements.</p>


      <a href="/blog/swift-5.4-released/">Read more...</a>
    </section>
  </article>

  <article id="/blog/swift-collections" class="summary">
    <header>
      <h1 class="title"><a href="/blog/swift-collections/">Introducing Swift Collections</a></h1>
      <time pubdate datetime="2021-04-05T10:00:00-04:00">April 5, 2021</time>
    </header>
    <section class="excerpt">
      <p>I’m thrilled to announce <a href="https://github.com/apple/swift-collections">Swift Collections</a>, a new open-source package focused on extending the set of available Swift data structures. Like the <a href="https://github.com/apple/swift-algorithms">Swift Algorithms</a> and <a href="https://github.com/apple/swift-numerics">Swift Numerics</a> packages before it, we’re releasing Swift Collections to help incubate new functionality for the Swift Standard Library.</p>



      <a href="/blog/swift-collections/">Read more...</a>
    </section>
  </article>

  <article id="/blog/womens-history-month" class="summary">
    <header>
      <h1 class="title"><a href="/blog/womens-history-month/">Celebrating Women’s History Month</a></h1>
      <time pubdate datetime="2021-03-24T06:00:00-04:00">March 24, 2021</time>
    </header>
    <section class="excerpt">
      <p>This Women’s History Month, we’re so happy to celebrate the amazing women developers in our community. Women have made an immense impact on the Swift ecosystem by building important tools we use every day, creating resources to pass on what they have learned, and more. This post highlights a few outstanding contributions from individuals in the Women in Swift community.</p>


      <a href="/blog/womens-history-month/">Read more...</a>
    </section>
  </article>

  <article id="/blog/black-history-month" class="summary">
    <header>
      <h1 class="title"><a href="/blog/black-history-month/">Celebrating Black History Month</a></h1>
      <time pubdate datetime="2021-02-22T06:00:00-04:00">February 22, 2021</time>
    </header>
    <section class="excerpt">
      <p>Black History Month is a time to learn about, reflect on, and celebrate the impact and accomplishments of the Black community. In honor of Black History Month, we have curated a handful of outstanding contributions from the Black Swift community to acknowledge and celebrate their impact on the Swift ecosystem.</p>


      <a href="/blog/black-history-month/">Read more...</a>
    </section>
  </article>

  <article id="/blog/diversity-in-swift" class="summary">
    <header>
      <h1 class="title"><a href="/blog/diversity-in-swift/">Diversity in Swift</a></h1>
      <time pubdate datetime="2020-12-16T06:00:00-04:00">December 16, 2020</time>
    </header>
    <section class="excerpt">
      <p>6 years ago, Swift was announced.  In the years since, a thriving community has emerged around a shared passion for building and using the Swift programming language. This community has spread far beyond Apple through conferences, open source repositories, community-authored books, and more — people are always finding new ways to connect with and support other Swift developers around the world. However, we feel we can always do more to encourage a wider range of developers to actively engage in our community.  That’s why we’re excited to announce <strong>Diversity in Swift</strong>. This initiative is focused on further elevating a wide variety of voices, and making it easier for developers to start learning or contributing to Swift, regardless of their background.</p>


      <a href="/blog/diversity-in-swift/">Read more...</a>
    </section>
  </article>

  <article id="/blog/accessibility-and-inclusion" class="summary">
    <header>
      <h1 class="title"><a href="/blog/accessibility-and-inclusion/">Accessibility and Inclusion in the Swift Community</a></h1>
      <time pubdate datetime="2020-12-16T05:59:00-04:00">December 16, 2020</time>
    </header>
    <section class="excerpt">
      <p>Diversity and inclusion are both critically important values when writing software designed to be used and enjoyed by everyone. The Swift community embraces these values, and we are excited to highlight ways to make sure everyone feels welcome, and bring even more people into the fold of Swift development.</p>


      <a href="/blog/accessibility-and-inclusion/">Read more...</a>
    </section>
  </article>

  <article id="/blog/swiftnio-ssh" class="summary">
    <header>
      <h1 class="title"><a href="/blog/swiftnio-ssh/">Introducing SwiftNIO SSH</a></h1>
      <time pubdate datetime="2020-11-19T05:00:00-04:00">November 19, 2020</time>
    </header>
    <section class="excerpt">
      <p>I am delighted to introduce a new open source project for the Swift Server ecosystem, <a href="https://github.com/apple/swift-nio-ssh">SwiftNIO SSH</a>. Distributed as a Swift package, SwiftNIO SSH is designed to enable Swift developers to interact with the SSH network protocol.</p>


      <a href="/blog/swiftnio-ssh/">Read more...</a>
    </section>
  </article>

  <article id="/blog/swift-service-discovery" class="summary">
    <header>
      <h1 class="title"><a href="/blog/swift-service-discovery/">Introducing Swift Service Discovery</a></h1>
      <time pubdate datetime="2020-10-21T05:00:00-04:00">October 21, 2020</time>
    </header>
    <section class="excerpt">
      <p>It is my pleasure to announce a new open source project for the Swift Server ecosystem, <a href="https://github.com/apple/swift-service-discovery">Swift Service Discovery</a>. Service Discovery is a Swift package designed to establish a standard API that can be implemented by various <a href="https://en.wikipedia.org/wiki/Service_discovery">service discovery</a> backends such as DNS-based, key-value store, etc.</p>


      <a href="/blog/swift-service-discovery/">Read more...</a>
    </section>
  </article>

  <article id="/blog/swift-algorithms" class="summary">
    <header>
      <h1 class="title"><a href="/blog/swift-algorithms/">Announcing Swift Algorithms</a></h1>
      <time pubdate datetime="2020-10-07T05:00:00-04:00">October 7, 2020</time>
    </header>
    <section class="excerpt">
      <p>I’m excited to announce <a href="https://github.com/apple/swift-algorithms">Swift Algorithms</a>, a new open-source package of sequence and collection algorithms, along with their related types.</p>


      <a href="/blog/swift-algorithms/">Read more...</a>
    </section>
  </article>

  <article id="/blog/swift-atomics" class="summary">
    <header>
      <h1 class="title"><a href="/blog/swift-atomics/">Introducing Swift Atomics</a></h1>
      <time pubdate datetime="2020-10-01T10:00:00-04:00">October 1, 2020</time>
    </header>
    <section class="excerpt">
      <p>I’m delighted to announce Swift Atomics, a new open source package that enables direct use of low-level atomic operations in Swift code. The goal of this library is to enable intrepid systems programmers to start building synchronization constructs (such as concurrent data structures) directly in Swift.</p>


      <a href="/blog/swift-atomics/">Read more...</a>
    </section>
  </article>

  <article id="/blog/swift-system" class="summary">
    <header>
      <h1 class="title"><a href="/blog/swift-system/">Swift System is Now Open Source</a></h1>
      <time pubdate datetime="2020-09-25T05:00:00-04:00">September 25, 2020</time>
    </header>
    <section class="excerpt">
      <p>In June, Apple introduced Swift System, a new library for Apple platforms that provides idiomatic interfaces to system calls and low-level currency types. Today, I’m excited to announce that we’re open-sourcing <a href="https://github.com/apple/swift-system">System</a> and adding Linux support! Our vision is for System to eventually act as the single home for low-level system interfaces for all supported Swift platforms.</p>


      <a href="/blog/swift-system/">Read more...</a>
    </section>
  </article>

  <article id="/blog/swift-on-windows" class="summary">
    <header>
      <h1 class="title"><a href="/blog/swift-on-windows/">Introducing Swift on Windows</a></h1>
      <time pubdate datetime="2020-09-22T05:00:00-04:00">September 22, 2020</time>
    </header>
    <section class="excerpt">
      <p>The Swift project is introducing <a href="https://swift.org/download">new downloadable Swift toolchain images</a> for Windows!  These images contain development components needed to build and run Swift code on Windows.</p>


      <a href="/blog/swift-on-windows/">Read more...</a>
    </section>
  </article>

  <article id="/blog/swift-5.3-released" class="summary">
    <header>
      <h1 class="title"><a href="/blog/swift-5.3-released/">Swift 5.3 released!</a></h1>
      <time pubdate datetime="2020-09-16T05:00:00-04:00">September 16, 2020</time>
    </header>
    <section class="excerpt">
      <p>Swift 5.3 is now officially released! 🎉</p>


      <a href="/blog/swift-5.3-released/">Read more...</a>
    </section>
  </article>

  <article id="/blog/swift-cluster-membership" class="summary">
    <header>
      <h1 class="title"><a href="/blog/swift-cluster-membership/">Introducing Swift Cluster Membership</a></h1>
      <time pubdate datetime="2020-08-27T05:00:00-04:00">August 27, 2020</time>
    </header>
    <section class="excerpt">
      <p>It is my pleasure to announce a new open source project for the Swift Server ecosystem, <a href="https://www.github.com/apple/swift-cluster-membership">Swift Cluster Membership</a>. This library aims to help Swift grow in a new space of server applications: clustered multi-node distributed systems. With this library we provide reusable runtime-agnostic <em>membership protocol</em> implementations which can be adopted in various clustering use-cases.</p>


      <a href="/blog/swift-cluster-membership/">Read more...</a>
    </section>
  </article>

  <article id="/blog/swift-service-lifecycle" class="summary">
    <header>
      <h1 class="title"><a href="/blog/swift-service-lifecycle/">Introducing Swift Service Lifecycle</a></h1>
      <time pubdate datetime="2020-07-15T05:00:00-04:00">July 15, 2020</time>
    </header>
    <section class="excerpt">
      <p>It is my pleasure to announce a new open source project for the Swift server ecosystem, <a href="https://github.com/swift-server/swift-service-lifecycle">Swift Service Lifecycle</a>. Service Lifecycle is a Swift package designed to help server applications, also known as services, manage their startup and shutdown sequences.</p>


      <a href="/blog/swift-service-lifecycle/">Read more...</a>
    </section>
  </article>

  <article id="/blog/AWS-lambda-runtime" class="summary">
    <header>
      <h1 class="title"><a href="/blog/AWS-lambda-runtime/">Introducing Swift AWS Lambda Runtime</a></h1>
      <time pubdate datetime="2020-05-29T06:00:00-04:00">May 29, 2020</time>
    </header>
    <section class="excerpt">
      <p>It is my pleasure to announce a new open source project for the Swift Server ecosystem, <a href="https://github.com/swift-server/swift-aws-lambda-runtime/">Swift AWS Lambda Runtime</a>. Distributed as a Swift package, the Swift AWS Lambda Runtime is designed to help Swift developers build serverless functions for the <a href="https://aws.amazon.com/lambda/">Amazon Web Services Lambda platform</a>.</p>


      <a href="/blog/AWS-lambda-runtime/">Read more...</a>
    </section>
  </article>

  <article id="/blog/additional-linux-distros" class="summary">
    <header>
      <h1 class="title"><a href="/blog/additional-linux-distros/">Additional Linux Distributions</a></h1>
      <time pubdate datetime="2020-05-05T06:00:00-04:00">May 5, 2020</time>
    </header>
    <section class="excerpt">
      <p>It is my pleasure to announce a new set of Linux distributions officially supported by the Swift project. <a href="https://swift.org/download/">Swift.org</a> now offers downloadable toolchain and Docker images for the following new Linux distributions:</p>


      <a href="/blog/additional-linux-distros/">Read more...</a>
    </section>
  </article>

  <article id="/blog/5.3-release-process" class="summary">
    <header>
      <h1 class="title"><a href="/blog/5.3-release-process/">Swift 5.3 Release Process</a></h1>
      <time pubdate datetime="2020-03-25T06:00:00-04:00">March 25, 2020</time>
    </header>
    <section class="excerpt">
      <p>This post describes the goals, release process, and estimated schedule for <strong>Swift 5.3</strong>.</p>


      <a href="/blog/5.3-release-process/">Read more...</a>
    </section>
  </article>

  <article id="/blog/swift-5.2-released" class="summary">
    <header>
      <h1 class="title"><a href="/blog/swift-5.2-released/">Swift 5.2 Released!</a></h1>
      <time pubdate datetime="2020-03-24T06:00:00-04:00">March 24, 2020</time>
    </header>
    <section class="excerpt">
      <p>Swift 5.2 is now officially released!  🎉</p>


      <a href="/blog/swift-5.2-released/">Read more...</a>
    </section>
  </article>

  <article id="/blog/argument-parser" class="summary">
    <header>
      <h1 class="title"><a href="/blog/argument-parser/">Announcing ArgumentParser</a></h1>
      <time pubdate datetime="2020-02-27T05:00:00-04:00">February 27, 2020</time>
    </header>
    <section class="excerpt">
      <p>We’re delighted to announce <a href="https://github.com/apple/swift-argument-parser"><code class="language-plaintext highlighter-rouge">ArgumentParser</code></a>, a new open-source library that makes it straightforward — even enjoyable! — to parse command-line arguments in Swift.</p>


      <a href="/blog/argument-parser/">Read more...</a>
    </section>
  </article>

  <article id="/blog/preview-package" class="summary">
    <header>
      <h1 class="title"><a href="/blog/preview-package/">Standard Library Preview Package</a></h1>
      <time pubdate datetime="2020-02-18T05:00:00-04:00">February 18, 2020</time>
    </header>
    <section class="excerpt">
      <p>I’m excited to announce a new open-source package and an enhancement to the Swift Evolution process: the <a href="https://github.com/apple/swift-standard-library-preview">Standard Library Preview package</a>! The preview package provides access to functionality that has been accepted into the Swift standard library through the <a href="https://github.com/apple/swift-evolution/blob/master/process.md">Swift Evolution process</a>, but has not yet shipped as part of an official Swift release. This will allow us to incorporate feedback informed by real-world usage and remove many of the technical obstacles to contributing to the standard library.</p>


      <a href="/blog/preview-package/">Read more...</a>
    </section>
  </article>

  <article id="/blog/library-evolution" class="summary">
    <header>
      <h1 class="title"><a href="/blog/library-evolution/">Library Evolution in Swift</a></h1>
      <time pubdate datetime="2020-02-13T05:00:00-04:00">February 13, 2020</time>
    </header>
    <section class="excerpt">
      <p>Swift 5.0 introduced a stable binary interface on Apple platforms. This meant that apps built with the Swift 5.0 compiler can use the Swift runtime and standard library built into the operating system, and that existing apps will remain compatible with new versions of the Swift runtime in future operating system releases.</p>


      <a href="/blog/library-evolution/">Read more...</a>
    </section>
  </article>

  <article id="/blog/crypto" class="summary">
    <header>
      <h1 class="title"><a href="/blog/crypto/">Introducing Swift Crypto</a></h1>
      <time pubdate datetime="2020-02-03T05:00:00-04:00">February 3, 2020</time>
    </header>
    <section class="excerpt">
      <p>I’m thrilled to announce a new open-source project for the Swift ecosystem,
<a href="https://github.com/apple/swift-crypto">Swift Crypto</a>. Swift Crypto is a new
Swift package that brings the fantastic APIs of <a href="https://developer.apple.com/documentation/cryptokit">Apple
CryptoKit</a> to the wider
Swift community. This will allow Swift developers, regardless of the platform
on which they deploy their applications, to access these APIs for a common set
of cryptographic operations.</p>


      <a href="/blog/crypto/">Read more...</a>
    </section>
  </article>

  <article id="/blog/numerics" class="summary">
    <header>
      <h1 class="title"><a href="/blog/numerics/">Swift Numerics</a></h1>
      <time pubdate datetime="2019-11-07T06:00:00-04:00">November 7, 2019</time>
    </header>
    <section class="excerpt">
      <p>I’m excited to announce a new open-source project for the Swift ecosystem, <a href="https://github.com/apple/swift-numerics">Swift Numerics</a>!
Swift Numerics will provide the building blocks of numerical computing in Swift, as a set of fine-grained modules bundled together into a single Swift package.
My hope is that we can quickly fill some important gaps in the Standard Library’s existing APIs, and unlock new domains of programming to the Swift language.</p>


      <a href="/blog/numerics/">Read more...</a>
    </section>
  </article>

  <article id="/blog/sswg-update-2019" class="summary">
    <header>
      <h1 class="title"><a href="/blog/sswg-update-2019/">SSWG Annual Update</a></h1>
      <time pubdate datetime="2019-10-31T06:00:00-04:00">October 31, 2019</time>
    </header>
    <section class="excerpt">
      <p>The <a href="https://swift.org/server/">Swift Server Work Group</a> (SSWG) set out <a href="https://forums.swift.org/t/next-steps-for-the-swift-server-work-group/15816">12 months ago</a> to begin defining and prioritizing new efforts to address the needs of the Swift server community. Since then, we’ve been busy meeting regularly, working with the community, defining guidelines, writing Swift packages, voting on proposals, posting in the forums, and much more. We feel that we’ve made significant progress toward those goals we set out last year and we’d like to share a high-level update with you today.</p>


      <a href="/blog/sswg-update-2019/">Read more...</a>
    </section>
  </article>

  <article id="/blog/new-diagnostic-arch-overview" class="summary">
    <header>
      <h1 class="title"><a href="/blog/new-diagnostic-arch-overview/">New Diagnostic Architecture Overview</a></h1>
      <time pubdate datetime="2019-10-17T06:00:00-04:00">October 17, 2019</time>
    </header>
    <section class="excerpt">
      <p>Diagnostics play a very important role in a programming language experience. It’s vital for developer productivity that the compiler can produce proper guidance in any situation, especially incomplete or invalid code.</p>


      <a href="/blog/new-diagnostic-arch-overview/">Read more...</a>
    </section>
  </article>

  <article id="/blog/5.2-release-process" class="summary">
    <header>
      <h1 class="title"><a href="/blog/5.2-release-process/">Swift 5.2 Release Process</a></h1>
      <time pubdate datetime="2019-09-24T06:00:00-04:00">September 24, 2019</time>
    </header>
    <section class="excerpt">
      <p>This post describes the goals, release process, and estimated schedule for <strong>Swift 5.2</strong>.</p>


      <a href="/blog/5.2-release-process/">Read more...</a>
    </section>
  </article>

  <article id="/blog/swift-5.1-released" class="summary">
    <header>
      <h1 class="title"><a href="/blog/swift-5.1-released/">Swift 5.1 Released!</a></h1>
      <time pubdate datetime="2019-09-20T06:00:00-04:00">September 20, 2019</time>
    </header>
    <section class="excerpt">
      <p>Swift 5.1 is now officially released!</p>


      <a href="/blog/swift-5.1-released/">Read more...</a>
    </section>
  </article>

  <article id="/blog/tsan-support-on-linux" class="summary">
    <header>
      <h1 class="title"><a href="/blog/tsan-support-on-linux/">Thread Sanitizer for Swift on Linux</a></h1>
      <time pubdate datetime="2019-08-13T06:00:00-04:00">August 13, 2019</time>
    </header>
    <section class="excerpt">
      <p>Thread Sanitizer is now available on Linux as part of Swift 5.1! Head over to <a href="https://swift.org/download/#snapshots">Swift.org</a> and grab a Swift 5.1 Development snapshot to try it out.</p>


      <a href="/blog/tsan-support-on-linux/">Read more...</a>
    </section>
  </article>

  <article id="/blog/swift-5-released" class="summary">
    <header>
      <h1 class="title"><a href="/blog/swift-5-released/">Swift 5 Released!</a></h1>
      <time pubdate datetime="2019-03-25T08:00:00-04:00">March 25, 2019</time>
    </header>
    <section class="excerpt">
      <p>Swift 5 is now officially released!</p>


      <a href="/blog/swift-5-released/">Read more...</a>
    </section>
  </article>

  <article id="/blog/utf8-string" class="summary">
    <header>
      <h1 class="title"><a href="/blog/utf8-string/">UTF-8 String</a></h1>
      <time pubdate datetime="2019-03-20T06:00:00-04:00">March 20, 2019</time>
    </header>
    <section class="excerpt">
      <p>Swift 5 switches the preferred encoding of strings from UTF-16 to UTF-8 while preserving efficient Objective-C-interoperability. Because the String type abstracts away these low-level concerns, no source-code changes from developers should be necessary*, but it’s worth highlighting some of the benefits this move gives us now and in the future.</p>


      <a href="/blog/utf8-string/">Read more...</a>
    </section>
  </article>

  <article id="/blog/behind-SE-0200" class="summary">
    <header>
      <h1 class="title"><a href="/blog/behind-SE-0200/">Behind the Proposal — SE-0200 Enhancing String Literals Delimiters to Support Raw Text</a></h1>
      <time pubdate datetime="2019-02-20T06:00:00-04:00">February 20, 2019</time>
    </header>
    <section class="excerpt">
      <p>The development, refinement, and deployment of <a href="https://github.com/apple/swift-evolution/blob/master/proposals/0200-raw-string-escaping.md">SE-0200 Enhancing String Literals Delimiters to Support Raw Text</a> was a long and surprising journey. It ended with a uniquely Swift take on “raw strings” that focused on adding custom delimiters to string literals and escape sequences.</p>


      <a href="/blog/behind-SE-0200/">Read more...</a>
    </section>
  </article>

  <article id="/blog/5.1-release-process" class="summary">
    <header>
      <h1 class="title"><a href="/blog/5.1-release-process/">Swift 5.1 Release Process</a></h1>
      <time pubdate datetime="2019-02-18T06:00:00-04:00">February 18, 2019</time>
    </header>
    <section class="excerpt">
      <p>This post describes the goals, release process, and estimated schedule for <strong>Swift 5.1</strong>.</p>


      <a href="/blog/5.1-release-process/">Read more...</a>
    </section>
  </article>

  <article id="/blog/abi-stability-and-apple" class="summary">
    <header>
      <h1 class="title"><a href="/blog/abi-stability-and-apple/">Evolving Swift On Apple Platforms After ABI Stability</a></h1>
      <time pubdate datetime="2019-02-11T06:00:00-04:00">February 11, 2019</time>
    </header>
    <section class="excerpt">
      <p>With the release of Swift 5.0, Swift is now ABI stable and is delivered as a core component of macOS, iOS, tvOS, and watchOS. ABI stability has been a goal for Swift since its inception, and brings with it many benefits for developers and users of these platforms:</p>


      <a href="/blog/abi-stability-and-apple/">Read more...</a>
    </section>
  </article>

  <article id="/blog/abi-stability-and-more" class="summary">
    <header>
      <h1 class="title"><a href="/blog/abi-stability-and-more/">ABI Stability and More</a></h1>
      <time pubdate datetime="2019-02-07T06:00:00-04:00">February 7, 2019</time>
    </header>
    <section class="excerpt">
      <p>It has been a longstanding goal to stabilize Swift’s ABI on macOS, iOS, watchOS, and tvOS.  While a stable ABI is an important milestone for the maturity of any language, the ultimate benefit to the Swift ecosystem was to enable binary compatibility for apps and libraries.  This post describes what binary compatibility means in Swift 5 and how it will evolve in future releases of Swift.</p>


      <a href="/blog/abi-stability-and-more/">Read more...</a>
    </section>
  </article>

  <article id="/blog/sourcekitd-stress-tester" class="summary">
    <header>
      <h1 class="title"><a href="/blog/sourcekitd-stress-tester/">Introducing the sourcekitd Stress Tester</a></h1>
      <time pubdate datetime="2019-02-06T08:00:00-04:00">February 6, 2019</time>
    </header>
    <section class="excerpt">
      <p>Sourcekitd provides the data backing key editor features like code completion, semantic highlighting, and refactoring for Swift files in both Xcode and the recently announced <a href="https://forums.swift.org/t/introducing-sourcekit-lsp/17964">SourceKit-LSP</a>. To help improve its robustness, we’re introducing a new tool, the sourcekitd stress tester, that over the past few months has helped find 91 reproducible sourcekitd crashes, assertion failures, and hangs. This post covers the stress tester’s implementation, its deployment in Swift’s CI and PR testing, and how Swift developers can run it over their own projects to help improve the Swift editing experience for everyone.</p>


      <a href="/blog/sourcekitd-stress-tester/">Read more...</a>
    </section>
  </article>

  <article id="/blog/swift-5-exclusivity" class="summary">
    <header>
      <h1 class="title"><a href="/blog/swift-5-exclusivity/">Swift 5 Exclusivity Enforcement</a></h1>
      <time pubdate datetime="2019-02-05T06:00:00-04:00">February 5, 2019</time>
    </header>
    <section class="excerpt">
      <p>The Swift 5 release enables runtime checking of “Exclusive Access to
Memory” by default in Release builds, further enhancing Swift’s
capabilities as a safe language. In Swift 4, these runtime checks were
only enabled in Debug builds. In this post, I’ll first explain what
this change means for Swift developers before delving into why it is
essential to Swift’s strategy for safety and performance.</p>


      <a href="/blog/swift-5-exclusivity/">Read more...</a>
    </section>
  </article>

  <article id="/blog/swiftpm-repl-support" class="summary">
    <header>
      <h1 class="title"><a href="/blog/swiftpm-repl-support/">REPL Support for Swift Packages</a></h1>
      <time pubdate datetime="2018-10-08T06:00:00-04:00">October 8, 2018</time>
    </header>
    <section class="excerpt">
      <p>The <code class="language-plaintext highlighter-rouge">swift run</code> command has a new <code class="language-plaintext highlighter-rouge">--repl</code> option which launches the Swift REPL with support for importing library targets of a package.</p>


      <a href="/blog/swiftpm-repl-support/">Read more...</a>
    </section>
  </article>

  <article id="/blog/how-mirror-works" class="summary">
    <header>
      <h1 class="title"><a href="/blog/how-mirror-works/">How Mirror Works</a></h1>
      <time pubdate datetime="2018-09-26T06:00:00-04:00">September 26, 2018</time>
    </header>
    <section class="excerpt">
      <p>Swift places a lot of emphasis on static typing, but it also supports rich metadata about types, which allows code to inspect and manipulate arbitrary values at runtime. This is exposed to Swift programmers through the <code class="language-plaintext highlighter-rouge">Mirror</code> API. One might wonder, how does something like <code class="language-plaintext highlighter-rouge">Mirror</code> work in a language with so much emphasis on static types? Let’s take a look!</p>


      <a href="/blog/how-mirror-works/">Read more...</a>
    </section>
  </article>

  <article id="/blog/5.0-release-process" class="summary">
    <header>
      <h1 class="title"><a href="/blog/5.0-release-process/">Swift 5.0 Release Process</a></h1>
      <time pubdate datetime="2018-09-25T06:00:00-04:00">September 25, 2018</time>
    </header>
    <section class="excerpt">
      <p>This post describes the goals, release process, and estimated schedule for
<strong>Swift 5.0</strong>.</p>


      <a href="/blog/5.0-release-process/">Read more...</a>
    </section>
  </article>

  <article id="/blog/swift-4.2-released" class="summary">
    <header>
      <h1 class="title"><a href="/blog/swift-4.2-released/">Swift 4.2 Released!</a></h1>
      <time pubdate datetime="2018-09-17T06:00:00-04:00">September 17, 2018</time>
    </header>
    <section class="excerpt">
      <p>Swift 4.2 is now officially released!  Swift 4.2 builds on the strengths of Swift 4, delivering faster compile times, improving the debugging experience, updating the standard library, and converging on binary compatibility.</p>


      <a href="/blog/swift-4.2-released/">Read more...</a>
    </section>
  </article>

  <article id="/blog/related-projects" class="summary">
    <header>
      <h1 class="title"><a href="/blog/related-projects/">Introducing Related Projects to Swift Forums</a></h1>
      <time pubdate datetime="2018-05-10T06:00:00-04:00">May 10, 2018</time>
    </header>
    <section class="excerpt">
      <p>The Swift community is growing and <a href="https://forums.swift.org">Swift Forums</a> are growing with it.</p>


      <a href="/blog/related-projects/">Read more...</a>
    </section>
  </article>

  <article id="/blog/swift-community-hosted-CI" class="summary">
    <header>
      <h1 class="title"><a href="/blog/swift-community-hosted-CI/">Swift Community-Hosted Continuous Integration</a></h1>
      <time pubdate datetime="2018-05-03T05:00:00-04:00">May 3, 2018</time>
    </header>
    <section class="excerpt">
      <p>We are delighted to announce a significant expansion of our Swift.org continuous integration testing system.  Members of the Swift community have been hard at work to support Swift on a number of new platforms, and we have extended the Swift CI system to support community-hosted nodes for testing additional platforms.</p>


      <a href="/blog/swift-community-hosted-CI/">Read more...</a>
    </section>
  </article>

  <article id="/blog/iuo" class="summary">
    <header>
      <h1 class="title"><a href="/blog/iuo/">Reimplementation of Implicitly Unwrapped Optionals</a></h1>
      <time pubdate datetime="2018-04-26T05:00:00-04:00">April 26, 2018</time>
    </header>
    <section class="excerpt">
      <p>A new implementation of implicitly unwrapped optionals (IUOs) landed in the Swift compiler earlier this year and is available to try in recent Swift <a href="https://swift.org/download/#snapshots">snapshots</a>.
This completes the implementation of <a href="https://github.com/apple/swift-evolution/blob/master/proposals/0054-abolish-iuo.md">SE-0054 - Abolish ImplicitlyUnwrappedOptional Type</a>.
This is an important change to the language that eliminated some inconsistencies in type checking and clarified the rule of how these values are to be treated so that it is consistent and easy to reason about. For more information, see the <a href="https://github.com/apple/swift-evolution/blob/master/proposals/0054-abolish-iuo.md#motivation">motivation section</a> of that proposal.</p>


      <a href="/blog/iuo/">Read more...</a>
    </section>
  </article>

  <article id="/blog/swift-4.1-released" class="summary">
    <header>
      <h1 class="title"><a href="/blog/swift-4.1-released/">Swift 4.1 Released!</a></h1>
      <time pubdate datetime="2018-03-29T06:00:00-04:00">March 29, 2018</time>
    </header>
    <section class="excerpt">
      <p>Swift 4.1 is now officially released!  It contains updates to the core language, including more support for generics, new build options, as well as minor enhancements to Swift Package Manager and Foundation.  There was also significant progress made in stabilizing the ABI.</p>


      <a href="/blog/swift-4.1-released/">Read more...</a>
    </section>
  </article>

  <article id="/blog/4.2-release-process" class="summary">
    <header>
      <h1 class="title"><a href="/blog/4.2-release-process/">Swift 4.2 Release Process</a></h1>
      <time pubdate datetime="2018-02-28T06:00:00-04:00">February 28, 2018</time>
    </header>
    <section class="excerpt">
      <p>This post describes the goals, release process, and estimated schedule for
<strong>Swift 4.2</strong>.</p>


      <a href="/blog/4.2-release-process/">Read more...</a>
    </section>
  </article>

  <article id="/blog/osize" class="summary">
    <header>
      <h1 class="title"><a href="/blog/osize/">Code Size Optimization Mode in Swift 4.1</a></h1>
      <time pubdate datetime="2018-02-08T06:00:00-04:00">February 8, 2018</time>
    </header>
    <section class="excerpt">
      <p>In Swift 4.1 the compiler now supports a new optimization mode which enables dedicated optimizations to reduce code size.</p>


      <a href="/blog/osize/">Read more...</a>
    </section>
  </article>

  <article id="/blog/forums" class="summary">
    <header>
      <h1 class="title"><a href="/blog/forums/">Swift Forums Now Open!</a></h1>
      <time pubdate datetime="2018-01-19T14:00:00-04:00">January 19, 2018</time>
    </header>
    <section class="excerpt">
      <p>We are delighted to announce that the Swift project has completed the process of migrating to the <a href="https://forums.swift.org">Swift Forums</a> as the primary method for discussion and communication!  The former mailing lists have been shut down and <a href="https://lists.swift.org/mailman/listinfo">archived</a>, and all mailing list content has been imported into the new forum system.</p>


      <a href="/blog/forums/">Read more...</a>
    </section>
  </article>

  <article id="/blog/conditional-conformance" class="summary">
    <header>
      <h1 class="title"><a href="/blog/conditional-conformance/">Conditional Conformance in the Standard Library</a></h1>
      <time pubdate datetime="2018-01-08T08:00:00-04:00">January 8, 2018</time>
    </header>
    <section class="excerpt">
      <p>The Swift 4.1 compiler brings the next phase of improvements from the
<a href="https://github.com/apple/swift/blob/master/docs/GenericsManifesto.md">roadmap for generics</a>: <a href="https://github.com/apple/swift-evolution/blob/master/proposals/0143-conditional-conformances.md">conditional conformances</a>.</p>



      <a href="/blog/conditional-conformance/">Read more...</a>
    </section>
  </article>

  <article id="/blog/swift-4.1-release-process" class="summary">
    <header>
      <h1 class="title"><a href="/blog/swift-4.1-release-process/">Swift 4.1 Release Process</a></h1>
      <time pubdate datetime="2017-10-17T06:00:00-04:00">October 17, 2017</time>
    </header>
    <section class="excerpt">
      <p>This post describes the goals, release process, and estimated schedule for Swift 4.1.</p>


      <a href="/blog/swift-4.1-release-process/">Read more...</a>
    </section>
  </article>

  <article id="/blog/xcode-9.1-improves-display-of-fatal-errors" class="summary">
    <header>
      <h1 class="title"><a href="/blog/xcode-9.1-improves-display-of-fatal-errors/">Xcode 9.1 Improves Display of Fatal Errors</a></h1>
      <time pubdate datetime="2017-10-05T08:00:00-04:00">October 5, 2017</time>
    </header>
    <section class="excerpt">
      <p>Swift has language constructs that allow you to specify your program’s expectations. If these expectations are not met at runtime, the program will be terminated. For example, <em>indexing into an array</em> implicitly expresses an expectation that the index is in bounds:</p>


      <a href="/blog/xcode-9.1-improves-display-of-fatal-errors/">Read more...</a>
    </section>
  </article>

  <article id="/blog/dictionary-and-set-improvements" class="summary">
    <header>
      <h1 class="title"><a href="/blog/dictionary-and-set-improvements/">Dictionary and Set Improvements in Swift 4.0</a></h1>
      <time pubdate datetime="2017-10-04T08:00:00-04:00">October 4, 2017</time>
    </header>
    <section class="excerpt">
      <p>In the latest release of Swift,
dictionaries and sets gain a number of new methods and initializers
that make common tasks easier than ever.
Operations like grouping, filtering, and transforming values
can now be performed in a single step,
letting you write more expressive and efficient code.</p>


      <a href="/blog/dictionary-and-set-improvements/">Read more...</a>
    </section>
  </article>

  <article id="/blog/swift-4.0-released" class="summary">
    <header>
      <h1 class="title"><a href="/blog/swift-4.0-released/">Swift 4.0 Released!</a></h1>
      <time pubdate datetime="2017-09-19T08:00:00-04:00">September 19, 2017</time>
    </header>
    <section class="excerpt">
      <p>Swift 4 is now officially released!  Swift 4 builds on the strengths of Swift 3, delivering greater robustness and stability, providing source code compatibility with Swift 3, making improvements to the standard library, and adding features like archival and serialization.</p>


      <a href="/blog/swift-4.0-released/">Read more...</a>
    </section>
  </article>

  <article id="/blog/swift-local-refactoring" class="summary">
    <header>
      <h1 class="title"><a href="/blog/swift-local-refactoring/">Swift Local Refactoring</a></h1>
      <time pubdate datetime="2017-08-22T05:45:00-04:00">August 22, 2017</time>
    </header>
    <section class="excerpt">
      <p>Xcode 9 includes a brand new refactoring engine. It can transform code locally
within a single Swift source file, or globally, such as renaming a method or property
that occurs in multiple files and even different languages. The logic behind local refactorings is
implemented entirely in the compiler and SourceKit, and is now open source in
the <a href="https://github.com/apple/swift">swift repository</a>. Therefore, any Swift enthusiast can
contribute refactoring actions to the language. This post discusses how
a simple refactoring can be implemented and surfaced in Xcode.</p>


      <a href="/blog/swift-local-refactoring/">Read more...</a>
    </section>
  </article>

  <article id="/blog/swift-package-manager-manifest-api-redesign" class="summary">
    <header>
      <h1 class="title"><a href="/blog/swift-package-manager-manifest-api-redesign/">Swift Package Manager Manifest API Redesign</a></h1>
      <time pubdate datetime="2017-06-21T05:45:00-04:00">June 21, 2017</time>
    </header>
    <section class="excerpt">
      <p>The Package Manager in Swift 4 includes the redesigned <code class="language-plaintext highlighter-rouge">Package.swift</code> manifest
API.  The new API is easier to use and follows the <a href="https://swift.org/documentation/api-design-guidelines/">design guidelines</a>.  The target
inference rules in Swift 3 Package Manager were a common source of confusion. We
revised these rules and removed most of the inference, favoring the practice of
explicitly specifying package structure in the manifest.</p>


      <a href="/blog/swift-package-manager-manifest-api-redesign/">Read more...</a>
    </section>
  </article>

  <article id="/blog/swift-source-compatibility-test-suite" class="summary">
    <header>
      <h1 class="title"><a href="/blog/swift-source-compatibility-test-suite/">Swift Source Compatibility Test Suite Now Available</a></h1>
      <time pubdate datetime="2017-04-24T09:01:01-04:00">April 24, 2017</time>
    </header>
    <section class="excerpt">
      <p>We are pleased to announce the release of a new <a href="https://github.com/apple/swift-source-compat-suite">Swift source compatibility test
suite</a> as part of the effort
to maintain source compatibility in future Swift releases.</p>


      <a href="/blog/swift-source-compatibility-test-suite/">Read more...</a>
    </section>
  </article>

  <article id="/blog/swift-3.1-released" class="summary">
    <header>
      <h1 class="title"><a href="/blog/swift-3.1-released/">Swift 3.1 Released!</a></h1>
      <time pubdate datetime="2017-03-27T06:00:00-04:00">March 27, 2017</time>
    </header>
    <section class="excerpt">
      <p>Swift 3.1 is now officially released!  Swift 3.1 is a minor release that contains improvements and refinements to the Standard Library. Thanks to efforts by IBM and other members of the community, it also includes many updates to the Linux implementation of Swift.  There are also a number of updates to Swift Package Manager.</p>


      <a href="/blog/swift-3.1-released/">Read more...</a>
    </section>
  </article>

  <article id="/blog/swift-4.0-release-process" class="summary">
    <header>
      <h1 class="title"><a href="/blog/swift-4.0-release-process/">Swift 4 Release Process</a></h1>
      <time pubdate datetime="2017-02-16T06:00:00-04:00">February 16, 2017</time>
    </header>
    <section class="excerpt">
      <p>This post describes the goals, release process, and estimated schedule for Swift 4.</p>


      <a href="/blog/swift-4.0-release-process/">Read more...</a>
    </section>
  </article>

  <article id="/blog/bridging-pch" class="summary">
    <header>
      <h1 class="title"><a href="/blog/bridging-pch/">Faster Mix-and-Match Builds with Precompiled Bridging Headers</a></h1>
      <time pubdate datetime="2017-01-26T06:00:00-04:00">January 26, 2017</time>
    </header>
    <section class="excerpt">
      <p>An examination of build times of Xcode projects that mix Objective-C and Swift, which can contain large bridging headers, shows that the Swift compiler spends a lot of time re-processing the same bridging headers for all the Swift files in a project.
In certain projects, each additional Swift file increases the overall build time noticeably, even when the Swift file is quite modest.</p>


      <a href="/blog/bridging-pch/">Read more...</a>
    </section>
  </article>

  <article id="/blog/swift-evolution-status-page" class="summary">
    <header>
      <h1 class="title"><a href="/blog/swift-evolution-status-page/">Swift Evolution Status Page Now Available</a></h1>
      <time pubdate datetime="2017-01-18T08:01:01-04:00">January 18, 2017</time>
    </header>
    <section class="excerpt">
      <p>We’re pleased to announce the release of the new <a href="https://apple.github.io/swift-evolution/">Swift Evolution</a> status page as a one-stop destination for information about proposed changes to Swift.</p>


      <a href="/blog/swift-evolution-status-page/">Read more...</a>
    </section>
  </article>

  <article id="/blog/swift-3.1-release-process" class="summary">
    <header>
      <h1 class="title"><a href="/blog/swift-3.1-release-process/">Swift 3.1 Release Process</a></h1>
      <time pubdate datetime="2016-12-09T06:00:00-04:00">December 9, 2016</time>
    </header>
    <section class="excerpt">
      <p>This post describes the goals, release process, and estimated schedule for Swift 3.1.</p>


      <a href="/blog/swift-3.1-release-process/">Read more...</a>
    </section>
  </article>

  <article id="/blog/server-api-workgroup" class="summary">
    <header>
      <h1 class="title"><a href="/blog/server-api-workgroup/">Server APIs Work Group</a></h1>
      <time pubdate datetime="2016-10-25T05:00:00-04:00">October 25, 2016</time>
    </header>
    <section class="excerpt">
      <p>Since Swift became available on Linux there has been a huge amount of interest in using Swift on the server, resulting in the emergence of a number of Web Frameworks, including Kitura, Vapor, Perfect, and Zewo, along with many others. As an important part of the Swift ecosystem, and one that we are keen to foster, we are today announcing the formation of the Server APIs work group.</p>


      <a href="/blog/server-api-workgroup/">Read more...</a>
    </section>
  </article>

  <article id="/blog/whole-module-optimizations" class="summary">
    <header>
      <h1 class="title"><a href="/blog/whole-module-optimizations/">Whole-Module Optimization in Swift 3</a></h1>
      <time pubdate datetime="2016-10-21T06:00:00-04:00">October 21, 2016</time>
    </header>
    <section class="excerpt">
      <p>Whole-module optimization is an optimization mode of the Swift compiler.
The performance win of whole-module optimization heavily depends on the project, but it can be up to two or even five times.</p>


      <a href="/blog/whole-module-optimizations/">Read more...</a>
    </section>
  </article>

  <article id="/blog/swift-3.0-released" class="summary">
    <header>
      <h1 class="title"><a href="/blog/swift-3.0-released/">Swift 3.0 Released!</a></h1>
      <time pubdate datetime="2016-09-13T08:00:00-04:00">September 13, 2016</time>
    </header>
    <section class="excerpt">
      <p>Swift 3.0, the first major release of Swift since it was open-sourced, is now officially released!  Swift 3 is a huge release containing major improvements and  refinements to the core language and Standard Library, major additions to the Linux port of Swift, and the first official release of the <a href="https://swift.org/package-manager">Swift Package Manager</a>.</p>



      <a href="/blog/swift-3.0-released/">Read more...</a>
    </section>
  </article>

  <article id="/blog/swift-xcode-playground-support" class="summary">
    <header>
      <h1 class="title"><a href="/blog/swift-xcode-playground-support/">Xcode Playground Support</a></h1>
      <time pubdate datetime="2016-07-07T09:00:00-04:00">July 7, 2016</time>
    </header>
    <section class="excerpt">
      <p>We are delighted to introduce <a href="https://swift.org/lldb/#xcode-playground-support">Xcode Playground Support</a>
as part of the Swift open source community!</p>


      <a href="/blog/swift-xcode-playground-support/">Read more...</a>
    </section>
  </article>

  <article id="/blog/swift-3.0-preview-1-released" class="summary">
    <header>
      <h1 class="title"><a href="/blog/swift-3.0-preview-1-released/">Swift 3.0 Preview 1 Released!</a></h1>
      <time pubdate datetime="2016-06-13T08:00:00-04:00">June 13, 2016</time>
    </header>
    <section class="excerpt">
      <p>We are very pleased to announce <strong>Developer Preview 1</strong> of Swift 3.0!</p>


      <a href="/blog/swift-3.0-preview-1-released/">Read more...</a>
    </section>
  </article>

  <article id="/blog/swift-2.3" class="summary">
    <header>
      <h1 class="title"><a href="/blog/swift-2.3/">Swift 2.3</a></h1>
      <time pubdate datetime="2016-06-12T22:00:00-04:00">June 12, 2016</time>
    </header>
    <section class="excerpt">
      <p>We are pleased to announce <strong>Swift 2.3</strong>!</p>


      <a href="/blog/swift-2.3/">Read more...</a>
    </section>
  </article>

  <article id="/blog/swift-3.0-release-process" class="summary">
    <header>
      <h1 class="title"><a href="/blog/swift-3.0-release-process/">Swift 3.0 Release Process</a></h1>
      <time pubdate datetime="2016-05-06T09:00:00-04:00">May 6, 2016</time>
    </header>
    <section class="excerpt">
      <p>This post describes the goals, release process, and estimated schedule for Swift
3.0.</p>


      <a href="/blog/swift-3.0-release-process/">Read more...</a>
    </section>
  </article>

  <article id="/blog/swift-2.2-new-features" class="summary">
    <header>
      <h1 class="title"><a href="/blog/swift-2.2-new-features/">New Features in Swift 2.2</a></h1>
      <time pubdate datetime="2016-03-30T04:00:00-04:00">March 30, 2016</time>
    </header>
    <section class="excerpt">
      <p>Swift 2.2 brings new syntax, new features, and some deprecations too.  It is an interim release before Swift 3 comes later this year <a href="https://swift.org/blog/swift-api-transformation/">with even bigger changes</a>, and the changes in Swift 2.2 align with the broader goals of Swift 3 to focus on gradually stabilizing the core language and Standard Library by adding missing features, refining what is already there, and removing what is no longer needed in the language.  All changes in Swift 2.2 went through the community-driven <a href="https://swift.org/contributing/#participating-in-the-swift-evolution-process">Swift evolution process</a> — where over 30 proposals have been submitted, reviewed, and accepted since Swift was open-sourced a few months ago.</p>


      <a href="/blog/swift-2.2-new-features/">Read more...</a>
    </section>
  </article>

  <article id="/blog/swift-2.2-released" class="summary">
    <header>
      <h1 class="title"><a href="/blog/swift-2.2-released/">Swift 2.2 Released!</a></h1>
      <time pubdate datetime="2016-03-21T08:00:00-04:00">March 21, 2016</time>
    </header>
    <section class="excerpt">
      <p>We are very pleased to announce the release of Swift 2.2!  This is the first official release of Swift since it was open-sourced on December 3, 2015.  Notably, the release includes contributions from 212 non-Apple contributors — changes that span from simple bug fixes to enhancements and alterations to the core language and Swift Standard Library.</p>


      <a href="/blog/swift-2.2-released/">Read more...</a>
    </section>
  </article>

  <article id="/blog/swift-commit-access" class="summary">
    <header>
      <h1 class="title"><a href="/blog/swift-commit-access/">Expanding Commit Access</a></h1>
      <time pubdate datetime="2016-02-29T04:00:00-04:00">February 29, 2016</time>
    </header>
    <section class="excerpt">
      <p>Now that the Swift Continuous Integration system is established and proven, we’d like to grant commit access on a more frequent basis to project contributors who have established a track record of good contributions.  If you would like commit access, please send an email to <a href="mailto:code-owners@swift.org">the code owners list</a> with a list of 5 non-trivial pull requests that we accepted without modifications.</p>


      <a href="/blog/swift-commit-access/">Read more...</a>
    </section>
  </article>

  <article id="/blog/swift-benchmark-suite" class="summary">
    <header>
      <h1 class="title"><a href="/blog/swift-benchmark-suite/">Swift Benchmark Suite now Available</a></h1>
      <time pubdate datetime="2016-02-08T14:00:00-04:00">February 8, 2016</time>
    </header>
    <section class="excerpt">
      <p>Apple’s Swift Team is happy to announce that Swift’s <a href="https://github.com/apple/swift/tree/master/benchmark">benchmark
suite</a> is now open
source.</p>


      <a href="/blog/swift-benchmark-suite/">Read more...</a>
    </section>
  </article>

  <article id="/blog/swift-CI" class="summary">
    <header>
      <h1 class="title"><a href="/blog/swift-CI/">Continuous Integration now Available</a></h1>
      <time pubdate datetime="2016-02-01T14:00:00-04:00">February 1, 2016</time>
    </header>
    <section class="excerpt">
      <p>We are excited to announce that we have rolled out <a href="https://swift.org/continuous-integration">continuous integration</a> (aka, CI) for the Swift project!</p>


      <a href="/blog/swift-CI/">Read more...</a>
    </section>
  </article>

  <article id="/blog/swift-api-transformation" class="summary">
    <header>
      <h1 class="title"><a href="/blog/swift-api-transformation/">It's Coming: the Great Swift API Transformation</a></h1>
      <time pubdate datetime="2016-01-29T00:00:00-04:00">January 29, 2016</time>
    </header>
    <section class="excerpt">
      <p>Cocoa, the Swift standard library, maybe even your own types and
methods—it’s all about to change, and you can help determine how.</p>


      <a href="/blog/swift-api-transformation/">Read more...</a>
    </section>
  </article>

  <article id="/blog/swift-2.2-release-process" class="summary">
    <header>
      <h1 class="title"><a href="/blog/swift-2.2-release-process/">Swift 2.2 Release Process</a></h1>
      <time pubdate datetime="2016-01-05T14:00:00-04:00">January 5, 2016</time>
    </header>
    <section class="excerpt">
      <p>This post describes the goals, release process, and estimated schedule
for Swift 2.2.</p>


      <a href="/blog/swift-2.2-release-process/">Read more...</a>
    </section>
  </article>

  <article id="/blog/swift-3-api-design" class="summary">
    <header>
      <h1 class="title"><a href="/blog/swift-3-api-design/">Swift 3 API Design Guidelines</a></h1>
      <time pubdate datetime="2015-12-03T08:01:01-04:00">December 3, 2015</time>
    </header>
    <section class="excerpt">
      <p>The design of commonly-used libraries has a large impact on the
overall feel of a programming language. Great libraries feel like an
extension of the language itself, and consistency across libraries
elevates the overall development experience. To aid in the
construction of great Swift libraries, one of the major <a href="https://github.com/apple/swift-evolution/blob/master/README.md" title="Swift 3 goals">goals for
Swift 3</a> is to define a set of API design guidelines
and to apply those design guidelines consistently.</p>



      <a href="/blog/swift-3-api-design/">Read more...</a>
    </section>
  </article>

  <article id="/blog/swift-linux-port" class="summary">
    <header>
      <h1 class="title"><a href="/blog/swift-linux-port/">The Swift Linux Port</a></h1>
      <time pubdate datetime="2015-12-03T07:01:01-04:00">December 3, 2015</time>
    </header>
    <section class="excerpt">
      <p>With the launch of the open source Swift project, we are also releasing
a port that works with the Linux operating system! You can build it from
the Swift sources or <a href="/download/">download pre-built binaries for Ubuntu</a>. The port
is still a work in progress but we’re happy to say that it is usable
today for experimentation. Currently x86_64 is the only supported
architecture on Linux.</p>



      <a href="/blog/swift-linux-port/">Read more...</a>
    </section>
  </article>

  <article id="/blog/welcome" class="summary">
    <header>
      <h1 class="title"><a href="/blog/welcome/">The Swift.org Blog</a></h1>
      <time pubdate datetime="2015-12-03T06:01:01-04:00">December 3, 2015</time>
    </header>
    <section class="excerpt">
      <p>Welcome to the blog on Swift.org! Today we launched the open source Swift project along with the Swift.org website.  We couldn’t be more excited to work together in an open community to find and fix issues, add enhancements, and bring Swift to new platforms.</p>


      <a href="/blog/welcome/">Read more...</a>
    </section>
  </article>


</main>

<footer role="contentinfo">
  <div class="footer-content">
    
    <p class="copyright">Copyright © 2022 Apple Inc. All rights reserved.</p>
    <p class="trademark">Swift and the Swift logo are trademarks of Apple Inc.</p>
    <p class="privacy">
      <a href="//www.apple.com/privacy/privacy-policy/">Privacy Policy</a>
      <a href="//www.apple.com/legal/privacy/en-ww/cookies/">Cookies</a>
    </p>
  </div>
  <div class="footer-other">
    <form
      class="color-scheme-toggle"
      role="radiogroup"
      tabindex="0"
      id="color-scheme-toggle"
    >
      <legend class="visuallyhidden">Color scheme preference</legend>
      <label for="scheme-light">
        <input id="scheme-light" type="radio" name="color-scheme-preference" value="light">
        <span class="color-scheme-toggle-label">Light</span>
      </label>
      <label for="scheme-dark">
        <input id="scheme-dark" type="radio" name="color-scheme-preference" value="dark">
        <span class="color-scheme-toggle-label">Dark</span>
      </label>
      <label for="scheme-auto" id="scheme-auto-wrapper">
        <input id="scheme-auto" type="radio" name="color-scheme-preference" value="auto">
        <span class="color-scheme-toggle-label">Auto</span>
      </label>
    </form>
    <aside>
      <a href="https://twitter.com/swiftlang" rel="nofollow" title="Follow @SwiftLang on Twitter"><i class="twitter"></i></a>
      <a href="/atom.xml" title="Subscribe to Site Updates"><i class="feed"></i></a>
    </aside>
  </div>
</footer>


<script src="/assets/javascripts/application.js"></script>
<!-- metrics -->
<script>
    /* RSID: */
    var s_account="awdswiftorg"
</script>
<script src="https://developer.apple.com/assets/metrics/scripts/analytics.js"></script>
<script>
    s.pageName= AC && AC.Tracking && AC.Tracking.pageName();

    /************* DO NOT ALTER ANYTHING BELOW THIS LINE ! **************/
    var s_code=s.t();if(s_code)document.write(s_code)
</script>
<!-- /metrics -->
</body>
</html>
"""

extension String {
  @inline(never)
  @available(macOS 15.0, iOS 18.0, watchOS 11.0, tvOS 18.0, visionOS 2.0, *)
  var _words: [Substring] {
    var result: [Substring] = []
    
    var i = startIndex
    
    while i < endIndex {
      let start = i
      let end = _wordIndex(after: i)
      
      let substr = self[start ..< end]
      result.append(substr)
      i = end
    }
    
    return result
  }
}

@available(macOS 15.0, iOS 18.0, watchOS 11.0, tvOS 18.0, visionOS 2.0, *)
public func run_iterateWords(_ n: Int) {
  for _ in 0 ..< n {
    blackHole(swiftOrgHTML._words)
  }
}
