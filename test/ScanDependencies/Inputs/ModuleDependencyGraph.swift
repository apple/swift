//===--------------- ModuleDependencyGraph.swift --------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
import Foundation

enum ModuleDependencyId: Hashable {
  case swift(String)
  case swiftPlaceholder(String)
  case swiftPrebuiltExternal(String)
  case clang(String)

  var moduleName: String {
    switch self {
    case .swift(let name): return name
    case .swiftPlaceholder(let name): return name
    case .swiftPrebuiltExternal(let name): return name
    case .clang(let name): return name
    }
  }
}

extension ModuleDependencyId: Codable {
  enum CodingKeys: CodingKey {
    case swift
    case swiftPlaceholder
    case swiftPrebuiltExternal
    case clang
  }

  init(from decoder: Decoder) throws {
    let container = try decoder.container(keyedBy: CodingKeys.self)
    do {
      let moduleName =  try container.decode(String.self, forKey: .swift)
      self = .swift(moduleName)
    } catch {
      do {
        let moduleName =  try container.decode(String.self, forKey: .swiftPlaceholder)
        self = .swiftPlaceholder(moduleName)
      } catch {
        do {
          let moduleName =  try container.decode(String.self, forKey: .swiftPrebuiltExternal)
          self = .swiftPrebuiltExternal(moduleName)
        } catch {
          let moduleName =  try container.decode(String.self, forKey: .clang)
          self = .clang(moduleName)
        }
      }
    }
  }

  func encode(to encoder: Encoder) throws {
    var container = encoder.container(keyedBy: CodingKeys.self)
    switch self {
      case .swift(let moduleName):
        try container.encode(moduleName, forKey: .swift)
      case .swiftPlaceholder(let moduleName):
        try container.encode(moduleName, forKey: .swiftPlaceholder)
      case .swiftPrebuiltExternal(let moduleName):
        try container.encode(moduleName, forKey: .swiftPrebuiltExternal)
      case .clang(let moduleName):
        try container.encode(moduleName, forKey: .clang)
    }
  }
}

/// Bridging header
struct BridgingHeader: Codable {
  var path: String
  var sourceFiles: [String]
  var moduleDependencies: [String]
}

/// Details specific to Swift modules.
struct SwiftModuleDetails: Codable {
  /// The module interface from which this module was built, if any.
  var moduleInterfacePath: String?

  /// The paths of potentially ready-to-use compiled modules for the interface.
  var compiledModuleCandidates: [String]?

  /// Options to the compile command
  var commandLine: [String]? = []

  /// To build a PCM to be used by this Swift module, we need to append these
  /// arguments to the generic PCM build arguments reported from the dependency
  /// graph.
  var extraPcmArgs: [String]

  /// A flag to indicate whether or not this module is a framework.
  var isFramework: Bool

   /// The bridging header info, if any.
  var bridgingHeader: BridgingHeader?  

  /// A set of Swift Overlays of Clang Module Dependencies
  var swiftOverlayDependencies: [ModuleDependencyId]?
}

/// Details specific to Swift placeholder dependencies.
struct SwiftPlaceholderModuleDetails: Codable {
  /// The path to the .swiftModuleDoc file.
  var moduleDocPath: String?

  /// The path to the .swiftSourceInfo file.
  var moduleSourceInfoPath: String?
}

/// Details specific to Swift externally-pre-built modules.
struct SwiftPrebuiltExternalModuleDetails: Codable {
  /// The path to the already-compiled module that must be used instead of
  /// generating a job to build this module.
  var compiledModulePath: String

  /// The path to the .swiftModuleDoc file.
  var moduleDocPath: String?

  /// The path to the .swiftSourceInfo file.
  var moduleSourceInfoPath: String?
}

/// Details specific to Clang modules.
struct ClangModuleDetails: Codable {
  /// The path to the module map used to build this module.
  public var moduleMapPath: String

  /// Set of PCM Arguments of depending modules which
  /// are covered by the directDependencies info of this module
  public var dependenciesCapturedPCMArgs: Set<[String]>?

  /// clang-generated context hash
  var contextHash: String

  /// Options to the compile command
  var commandLine: [String] = []
}

struct ModuleInfo: Codable {
  /// The path for the module.
  var modulePath: String

  /// The source files used to build this module.
  var sourceFiles: [String]?

  /// The set of direct module dependencies of this module.
  var directDependencies: [ModuleDependencyId]?

  /// Specific details of a particular kind of module.
  var details: Details

  /// Specific details of a particular kind of module.
  enum Details {
    /// Swift modules may be built from a module interface, and may have
    /// a bridging header.
    case swift(SwiftModuleDetails)

    /// Swift placeholder modules carry additional details that specify their
    /// module doc path and source info paths.
    case swiftPlaceholder(SwiftPlaceholderModuleDetails)

    /// Swift externally-prebuilt modules must communicate the path to pre-built binary artifacts
    case swiftPrebuiltExternal(SwiftPrebuiltExternalModuleDetails)

    /// Clang modules are built from a module map file.
    case clang(ClangModuleDetails)
  }
}

extension ModuleInfo.Details: Codable {
  enum CodingKeys: CodingKey {
    case swift
    case swiftPlaceholder
    case swiftPrebuiltExternal
    case clang
  }

  init(from decoder: Decoder) throws {
    let container = try decoder.container(keyedBy: CodingKeys.self)
    do {
      let details = try container.decode(SwiftModuleDetails.self, forKey: .swift)
      self = .swift(details)
    } catch {
      do {
        let details = try container.decode(SwiftPlaceholderModuleDetails.self,
                                           forKey: .swiftPlaceholder)
        self = .swiftPlaceholder(details)
      } catch {
        do {
          let details = try container.decode(SwiftPrebuiltExternalModuleDetails.self,
                                             forKey: .swiftPrebuiltExternal)
          self = .swiftPrebuiltExternal(details)
        } catch {
          let details = try container.decode(ClangModuleDetails.self, forKey: .clang)
          self = .clang(details)
        }
      }
    }
  }

  func encode(to encoder: Encoder) throws {
    var container = encoder.container(keyedBy: CodingKeys.self)
    switch self {
      case .swift(let details):
        try container.encode(details, forKey: .swift)
      case .swiftPlaceholder(let details):
        try container.encode(details, forKey: .swiftPlaceholder)
      case .swiftPrebuiltExternal(let details):
        try container.encode(details, forKey: .swiftPrebuiltExternal)
      case .clang(let details):
        try container.encode(details, forKey: .clang)
    }
  }
}

/// Describes the complete set of dependencies for a Swift module, including
/// all of the Swift and C modules and source files it depends on.
struct ModuleDependencyGraph: Codable {
  /// The name of the main module.
  var mainModuleName: String

  /// The complete set of modules discovered
  var modules: [ModuleDependencyId: ModuleInfo] = [:]

  /// Information about the main module.
  var mainModule: ModuleInfo { modules[.swift(mainModuleName)]! }
}
