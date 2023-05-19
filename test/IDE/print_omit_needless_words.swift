// RUN: %empty-directory(%t)

// REQUIRES: objc_interop
// FIXME: this is failing on simulators
// REQUIRES: OS=macosx
// REQUIRES: asserts

// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk-nosource -I %t) -emit-module -o %t %S/../Inputs/clang-importer-sdk/swift-modules/ObjectiveC.swift -disable-objc-attr-requires-foundation-module -enable-experimental-feature SendableCompletionHandlers
// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk-nosource -I %t) -emit-module -o %t %S/../Inputs/clang-importer-sdk/swift-modules/CoreGraphics.swift -enable-experimental-feature SendableCompletionHandlers
// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk-nosource -I %t) -emit-module -o %t  %S/../Inputs/clang-importer-sdk/swift-modules/Foundation.swift -enable-experimental-feature SendableCompletionHandlers

// RUN: %target-swift-ide-test(mock-sdk: -sdk %S/../Inputs/clang-importer-sdk -I %t) -print-module -source-filename %s -module-to-print=ObjectiveC -function-definitions=false -prefer-type-repr=true -enable-experimental-feature SendableCompletionHandlers  > %t.ObjectiveC.txt
// RUN: %FileCheck %s -check-prefix=CHECK-OBJECTIVEC -strict-whitespace < %t.ObjectiveC.txt

// RUN: %target-swift-ide-test(mock-sdk: -sdk %S/../Inputs/clang-importer-sdk -I %t) -print-module -source-filename %s -module-to-print=Foundation -function-definitions=false -prefer-type-repr=true -skip-unavailable -skip-parameter-names -enable-experimental-feature SendableCompletionHandlers > %t.Foundation.txt
// RUN: %FileCheck %s -check-prefix=CHECK-FOUNDATION -strict-whitespace < %t.Foundation.txt

// RUN: %target-swift-ide-test(mock-sdk: -sdk %S/../Inputs/clang-importer-sdk -I %t -I %S/../ClangImporter/Inputs/custom-modules) -print-module -source-filename %s -module-to-print=CoreCooling -function-definitions=false -prefer-type-repr=true  -skip-parameter-names -enable-experimental-feature SendableCompletionHandlers > %t.CoreCooling.txt
// RUN: %FileCheck %s -check-prefix=CHECK-CORECOOLING -strict-whitespace < %t.CoreCooling.txt

// RUN: %target-swift-ide-test(mock-sdk: -sdk %S/../Inputs/clang-importer-sdk -I %t -I %S/Inputs/custom-modules) -print-module -source-filename %s -module-to-print=OmitNeedlessWords -function-definitions=false -prefer-type-repr=true  -skip-parameter-names -enable-experimental-feature SendableCompletionHandlers > %t.OmitNeedlessWords.txt 2> %t.OmitNeedlessWords.diagnostics.txt
// RUN: %FileCheck %s -check-prefix=CHECK-OMIT-NEEDLESS-WORDS -strict-whitespace < %t.OmitNeedlessWords.txt
// RUN: %FileCheck %s -check-prefix=CHECK-OMIT-NEEDLESS-WORDS-DIAGS -strict-whitespace < %t.OmitNeedlessWords.diagnostics.txt


// RUN: %target-swift-ide-test(mock-sdk: -sdk %S/../Inputs/clang-importer-sdk -I %t) -print-module -source-filename %s -module-to-print=errors -function-definitions=false -prefer-type-repr=true  -skip-parameter-names -enable-experimental-feature SendableCompletionHandlers > %t.errors.txt
// RUN: %FileCheck %s -check-prefix=CHECK-ERRORS -strict-whitespace < %t.errors.txt

// Note: SEL -> "Selector"
// CHECK-FOUNDATION: func makeObjectsPerform(_: Selector)

// Note: "with" parameters.
// CHECK-FOUNDATION: func makeObjectsPerform(_: Selector, with: Any?)
// CHECK-FOUNDATION: func makeObjectsPerform(_: Selector, with: Any?, with: Any?)

// Note: don't prefix-strip swift_bridged classes or their subclasses.
// CHECK-FOUNDATION: func mutableCopy() -> NSMutableArray

// Note: id -> "Object".
// CHECK-FOUNDATION: func index(of: Any) -> Int

// Note: Class -> "Class"
// CHECK-OBJECTIVEC: func isKind(of aClass: AnyClass) -> Bool

// Note: Pointer-to-struct name matching; preposition splitting.
//
// CHECK-FOUNDATION: func copy(with: NSZone? = nil) -> Any!

// Note: Objective-C type parameter names.
// CHECK-FOUNDATION: func object(forKey: Any) -> Any?
// CHECK-FOUNDATION: func removeObject(forKey: any NSCopying)

// Note: Don't drop the name of the first parameter in an initializer entirely.
// CHECK-FOUNDATION: init(array: [Any])

// Note: struct name matching; don't drop "With".
// CHECK-FOUNDATION: func withRange(_: NSRange) -> NSValue

// Note: built-in types.
// CHECK-FOUNDATION: func add(_: Double) -> NSNumber

// Note: built-in types.
// CHECK-FOUNDATION: func add(_: Bool) -> NSNumber

// Note: builtin-types.
// CHECK-FOUNDATION: func add(_: UInt16) -> NSNumber

// Note: builtin-types.
// CHECK-FOUNDATION: func add(_: Int32) -> NSNumber

// Note: Typedefs with a "_t" suffix".
// CHECK-FOUNDATION: func subtract(_: Int32) -> NSNumber

// Note: Respect the getter name for BOOL properties.
// CHECK-FOUNDATION: var isMakingHoney: Bool

// Note: multi-word enum name matching; "with" splits the first piece.
// CHECK-FOUNDATION: func someMethod(deprecatedOptions: NSDeprecatedOptions = [])

// Note: class name matching; don't drop "With".
// CHECK-FOUNDATION: func withString(_: String!) -> Self!

// Note: lowercasing enum constants.
// CHECK-FOUNDATION: enum CountStyle : Int {
// CHECK-FOUNDATION: case file
// CHECK-FOUNDATION-NEXT: case memory
// CHECK-FOUNDATION-NEXT: case decimal
// CHECK-FOUNDATION-NEXT: case binary

// Note: Make sure initialisms work in various places
// CHECK-FOUNDATION: open(_: URL!, completionHandler: (@Sendable (Bool) -> Void)!)
// CHECK-FOUNDATION: open(_: NSGUID!, completionHandler: (@Sendable (Bool) -> Void)!)

// Note: property name stripping property type.
// CHECK-FOUNDATION: var uppercased: String

// Note: ok to map base name down to a keyword.
// CHECK-FOUNDATION: func `do`(_: Selector)

// Note: Strip names preceded by a gerund.
// CHECK-FOUNDATION: func startSquashing(_: Bee)
// CHECK-FOUNDATION: func startSoothing(_: Bee)
// CHECK-FOUNDATION: func startShopping(_: Bee)

// Note: Removing plural forms when working with collections
// CHECK-FOUNDATION: func add(_: [Any])

// Note: Int and Index match.
// CHECK-FOUNDATION: func slice(from: Int, to: Int) -> String

// Note: <context type>By<gerund> --> <gerund>.
// CHECK-FOUNDATION: func appending(_: String) -> String

// Note: <context type>By<gerund> --> <gerund>.
// CHECK-FOUNDATION: func withString(_: String) -> String

// Note: Noun phrase puts preposition inside.
// CHECK-FOUNDATION: func url(withAddedString: String) -> URL?
// CHECK-FOUNDATION: func guid(withAddedString: String) -> NSGUID?

// Note: NSCalendarUnits is not a set of "Options".
// CHECK-FOUNDATION: func forCalendarUnits(_: NSCalendar.Unit) -> String!

// Note: <property type>By<gerund> --> <gerund>.
// Note: <property type><preposition> --> <preposition>.
// CHECK-FOUNDATION: var deletingLastPathComponent: URL? { get }
// CHECK-FOUNDATION: var withHTTPS: URL { get }
// CHECK-FOUNDATION: var canonicalizing: NSGUID? { get }
// CHECK-FOUNDATION: var withContext: NSGUID { get }

// Note: lowercasing option set values
// CHECK-FOUNDATION: struct NSEnumerationOptions
// CHECK-FOUNDATION: static var concurrent: NSEnumerationOptions
// CHECK-FOUNDATION: static var reverse: NSEnumerationOptions

// Note: usingBlock -> body
// CHECK-FOUNDATION: func enumerateObjects(_: ((Any?, Int, UnsafeMutablePointer<ObjCBool>?) -> Void)!)
// CHECK-FOUNDATION: func enumerateObjects(options: NSEnumerationOptions = [], using: ((Any?, Int, UnsafeMutablePointer<ObjCBool>?) -> Void)!)

// Note: WithBlock -> body, nullable closures default to nil.
// CHECK-FOUNDATION: func enumerateObjectsRandomly(block: (@Sendable (Any?, Int, UnsafeMutablePointer<ObjCBool>?) -> Void)? = nil)

// Note: id<Proto> treated as "Proto".
// CHECK-FOUNDATION: func doSomething(with: any NSCopying)

// Note: NSObject<Proto> treated as "Proto".
// CHECK-FOUNDATION: func doSomethingElse(with: any NSCopying & NSObjectProtocol)

// Note: Function type -> "Function".
// CHECK-FOUNDATION: func sort(_: @convention(c) (Any, Any) -> Int)

// Note: Plural: NSArray without type arguments -> "Objects".
// CHECK-FOUNDATION: func remove(_: [Any])

// Note: Skipping "Type" suffix.
// CHECK-FOUNDATION: func doSomething(with: NSUnderlyingType)

// Don't introduce default arguments for lone parameters to setters.
// CHECK-FOUNDATION: func setDefaultEnumerationOptions(_: NSEnumerationOptions)

// CHECK-FOUNDATION: func normalizingXMLPreservingComments(_: Bool)

// Collection element types.
// CHECK-FOUNDATION: func adding(_: Any) -> Set<AnyHashable>

// Boolean properties follow the getter.
// CHECK-FOUNDATION: var empty: Bool { get }
// CHECK-FOUNDATION: func nonEmpty() -> Bool
// CHECK-FOUNDATION: var isStringSet: Bool { get }
// CHECK-FOUNDATION: var wantsAUnion: Bool { get }
// CHECK-FOUNDATION: var watchesItsLanguage: Bool { get }
// CHECK-FOUNDATION: var appliesForAJob: Bool { get }
// CHECK-FOUNDATION: var setShouldBeInfinite: Bool { get }

// "UTF8" initialisms.
// CHECK-FOUNDATION: init?(utf8String: UnsafePointer<CChar>!)

// Don't strip prefixes from globals.
// CHECK-FOUNDATION: let NSGlobalConstant: String
// CHECK-FOUNDATION: func NSGlobalFunction()

// Cannot strip because we end up with something that isn't an identifier
// CHECK-FOUNDATION: func NS123()

// CHECK-FOUNDATION: func NSYELLING()
// CHECK-FOUNDATION: func NS_SCREAMING()
// CHECK-FOUNDATION: func NS_()
// CHECK-FOUNDATION: let NSHTTPRequestKey: String

// Lowercasing initialisms with plurals.
// CHECK-FOUNDATION: var urlsInText: [URL] { get }
// CHECK-FOUNDATION: var guidsInText: [NSGUID] { get }

// Don't strip prefixes from macro names.
// CHECK-FOUNDATION: var NSTimeIntervalSince1970: Double { get }
// CHECK-FOUNDATION: var NS_DO_SOMETHING: Int

// Note: no lowercasing of initialisms when there might be a prefix.
// CHECK-CORECOOLING: func CFBottom() ->

// Note: Skipping over "Ref"
// CHECK-CORECOOLING: func replace(_: CCPowerSupply!)

// CHECK-OMIT-NEEDLESS-WORDS: struct OMWWobbleOptions
// CHECK-OMIT-NEEDLESS-WORDS:   static var sideToSide: OMWWobbleOptions
// CHECK-OMIT-NEEDLESS-WORDS:   static var backAndForth: OMWWobbleOptions
// CHECK-OMIT-NEEDLESS-WORDS:   static var toXMLHex: OMWWobbleOptions

// CHECK-OMIT-NEEDLESS-WORDS: func jump(to: URL)
// CHECK-OMIT-NEEDLESS-WORDS: func jump(to: NSGUID)
// CHECK-OMIT-NEEDLESS-WORDS: func jumpAgain(to: NSGUID)
// CHECK-OMIT-NEEDLESS-WORDS: func objectIs(compatibleWith: Any) -> Bool
// CHECK-OMIT-NEEDLESS-WORDS: func insetBy(x: Int, y: Int)
// CHECK-OMIT-NEEDLESS-WORDS: func setIndirectlyToValue(_: Any)
// CHECK-OMIT-NEEDLESS-WORDS: func jumpToTop(_: Any)
// CHECK-OMIT-NEEDLESS-WORDS: func removeWithNoRemorse(_: Any)
// CHECK-OMIT-NEEDLESS-WORDS: func bookmark(with: [URL])
// CHECK-OMIT-NEEDLESS-WORDS: func save(to: URL, forSaveOperation: Int)
// CHECK-OMIT-NEEDLESS-WORDS: func save(to: NSGUID, forSaveOperation: Int)
// CHECK-OMIT-NEEDLESS-WORDS: func index(withItemNamed: String)
// CHECK-OMIT-NEEDLESS-WORDS: func methodAndReturnError(_: AutoreleasingUnsafeMutablePointer<NSError?>!)

// CHECK-OMIT-NEEDLESS-WORDS: func type(of: String)
// CHECK-OMIT-NEEDLESS-WORDS: func type(ofNamedString: String)
// CHECK-OMIT-NEEDLESS-WORDS: func type(ofTypeNamed: String)

// Look for preposition prior to "of".
// CHECK-OMIT-NEEDLESS-WORDS: func append(withContentsOf: String)

// Leave subscripts alone
// CHECK-OMIT-NEEDLESS-WORDS: subscript(_: UInt) -> Any { get }
// CHECK-OMIT-NEEDLESS-WORDS: func objectAtIndexedSubscript(_: UInt) -> Any

// CHECK-OMIT-NEEDLESS-WORDS: func exportPresets(bestMatching: String)
// CHECK-OMIT-NEEDLESS-WORDS: func `is`(compatibleWith: String)

// CHECK-OMIT-NEEDLESS-WORDS: func add(_: Any)

// CHECK-OMIT-NEEDLESS-WORDS: func slobbering(_: String) -> OmitNeedlessWords

// Elements of C array types
// CHECK-OMIT-NEEDLESS-WORDS: func drawPolygon(with: UnsafePointer<NSPoint>!, count: Int)

// Typedef ending in "Array".
// CHECK-OMIT-NEEDLESS-WORDS: func drawFilledPolygon(with: NSPointArray!, count: Int)

// Non-parameterized Objective-C class ending in "Array".
// CHECK-OMIT-NEEDLESS-WORDS: func draw(_: SEGreebieArray)

// "bound by"
// CHECK-OMIT-NEEDLESS-WORDS: func doSomething(boundBy: Int)

// "separated by"
// CHECK-OMIT-NEEDLESS-WORDS: func doSomething(separatedBy: Int)

// "Property"-like stripping for "set" methods.
// CHECK-OMIT-NEEDLESS-WORDS: class func current() -> OmitNeedlessWords
// CHECK-OMIT-NEEDLESS-WORDS: class func setCurrent(_: OmitNeedlessWords)

// Don't split "PlugIn".
// CHECK-OMIT-NEEDLESS-WORDS: func compilerPlugInValue(_: Int)

// Don't strip away argument label completely when there is a default
// argument.
// CHECK-OMIT-NEEDLESS-WORDS: func wobble(options: OMWWobbleOptions = [])

// Property-name sensitivity in the base name "Self" stripping.
// CHECK-OMIT-NEEDLESS-WORDS: func addDoodle(_: ABCDoodle)

// Protocols as contexts
// CHECK-OMIT-NEEDLESS-WORDS-LABEL: protocol OMWLanding {
// CHECK-OMIT-NEEDLESS-WORDS-NEXT: func flip()

// Verify that we get the Swift name from the original declaration.
// CHECK-OMIT-NEEDLESS-WORDS-LABEL: protocol OMWWiggle
// CHECK-OMIT-NEEDLESS-WORDS-NEXT: func joinSub()
// CHECK-OMIT-NEEDLESS-WORDS-NEXT: var wiggleProp1: Int { get }
// CHECK-OMIT-NEEDLESS-WORDS-NEXT: @available(swift, obsoleted: 3, renamed: "wiggleProp1")
// CHECK-OMIT-NEEDLESS-WORDS-NEXT: var conflictingProp1: Int { get }

// CHECK-OMIT-NEEDLESS-WORDS-LABEL: protocol OMWWaggle
// CHECK-OMIT-NEEDLESS-WORDS-NEXT: var waggleProp1: Int { get }
// CHECK-OMIT-NEEDLESS-WORDS-NEXT: @available(swift, obsoleted: 3, renamed: "waggleProp1")
// CHECK-OMIT-NEEDLESS-WORDS-NEXT: var conflictingProp1: Int { get }

// CHECK-OMIT-NEEDLESS-WORDS-LABEL: class OMWSuper
// CHECK-OMIT-NEEDLESS-WORDS-NEXT: func jump()
// CHECK-OMIT-NEEDLESS-WORDS-NEXT: @available(swift, obsoleted: 3, renamed: "jump()")
// CHECK-OMIT-NEEDLESS-WORDS-NEXT: func jumpSuper()
// CHECK-OMIT-NEEDLESS-WORDS-NEXT: var wiggleProp1: Int { get }

// CHECK-OMIT-NEEDLESS-WORDS-LABEL: class OMWSub
// CHECK-OMIT-NEEDLESS-WORDS-NEXT: func jump()
// CHECK-OMIT-NEEDLESS-WORDS-NEXT: @available(swift, obsoleted: 3, renamed: "jump()")
// CHECK-OMIT-NEEDLESS-WORDS-NEXT: func jumpSuper()
// CHECK-OMIT-NEEDLESS-WORDS-NEXT: func joinSub()

// CHECK-OMIT-NEEDLESS-WORDS-LABEL: class OMWObjectType
// CHECK-OMIT-NEEDLESS-WORDS-NEXT: func _enumerateTypes(handler: @escaping () -> Void)

// CHECK-OMIT-NEEDLESS-WORDS-LABEL: class OMWTerrifyingGarbage4DTypeRefMask_t
// CHECK-OMIT-NEEDLESS-WORDS-NEXT: func throwAway()
// CHECK-OMIT-NEEDLESS-WORDS-NEXT: @available(swift, obsoleted: 3, renamed: "throwAway()")
// CHECK-OMIT-NEEDLESS-WORDS-NEXT: func throwGarbageAway()
// CHECK-OMIT-NEEDLESS-WORDS-NEXT: func throwGarbage4DAwayHarder()
// CHECK-OMIT-NEEDLESS-WORDS-NEXT: func throwGarbage4DTypeRefMask_tAwayHardest()
// CHECK-OMIT-NEEDLESS-WORDS-NEXT: func burn()
// CHECK-OMIT-NEEDLESS-WORDS-NEXT: @available(swift, obsoleted: 3, renamed: "burn()")
// CHECK-OMIT-NEEDLESS-WORDS-NEXT: func burnGarbage()
// CHECK-OMIT-NEEDLESS-WORDS-NEXT: func carefullyBurn()
// CHECK-OMIT-NEEDLESS-WORDS-NEXT: @available(swift, obsoleted: 3, renamed: "carefullyBurn()")
// CHECK-OMIT-NEEDLESS-WORDS-NEXT: func carefullyBurnGarbage4D()
// CHECK-OMIT-NEEDLESS-WORDS-NEXT: func veryCarefullyBurn()
// CHECK-OMIT-NEEDLESS-WORDS-NEXT: @available(swift, obsoleted: 3, renamed: "veryCarefullyBurn()")
// CHECK-OMIT-NEEDLESS-WORDS-NEXT: func veryCarefullyBurnGarbage4DTypeRefMask_t()

// CHECK-OMIT-NEEDLESS-WORDS-DIAGS: inconsistent Swift name for Objective-C property 'conflictingProp1' in 'OMWSub' ('waggleProp1' in 'OMWWaggle' vs. 'wiggleProp1' in 'OMWSuper')

// Don't drop the 'error'.
// CHECK-ERRORS: func tryAndReturnError(_: ()) throws
