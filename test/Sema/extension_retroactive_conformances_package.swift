// RUN: %empty-directory(%t)
// RUN: split-file %s %t

// RUN: %target-swift-frontend -swift-version 5 %t/Library.swift -emit-module -module-name Library -o %t -package-name Library 
// RUN: %target-swift-frontend -swift-version 5 %t/OtherLibrary.swift -emit-module -module-name OtherLibrary -o %t -package-name Library
// RUN: %target-swift-frontend -typecheck %t/Client.swift -module-name Client -verify -swift-version 5 -I %t -package-name Library 

//--- Library.swift

public class LibraryClass {}
public protocol LibraryProtocol {}
package class PackageLibraryClass {}
package protocol PackageLibraryProtocol {}

//--- OtherLibrary.swift

public class OtherLibraryClass {}
public protocol OtherLibraryProtocol {}
package class PackageOtherLibraryClass {}
package protocol PackageOtherLibraryProtocol {}

//--- Client.swift

package import Library
package import OtherLibrary

// These are all fine because all 3 of these modules are in the same package.

extension LibraryClass: LibraryProtocol {}
extension OtherLibraryClass: LibraryProtocol {}
extension LibraryClass: OtherLibraryProtocol {}

extension PackageLibraryClass: LibraryProtocol {}
extension PackageOtherLibraryClass: LibraryProtocol {}
extension PackageLibraryClass: OtherLibraryProtocol {}

extension LibraryClass: PackageLibraryProtocol {}
extension OtherLibraryClass: PackageLibraryProtocol {}
extension LibraryClass: PackageOtherLibraryProtocol {}

extension PackageLibraryClass: PackageLibraryProtocol {}
extension PackageOtherLibraryClass: PackageLibraryProtocol {}
extension PackageLibraryClass: PackageOtherLibraryProtocol {}