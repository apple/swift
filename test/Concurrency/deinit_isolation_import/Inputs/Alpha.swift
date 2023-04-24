import ObjectiveC

@objc open class RoundtripNonisolated: NSObject {}

@objc open class RoundtripIsolated: NSObject {
    @MainActor deinit {}
}

@objc open class RoundtripAsync: NSObject {
    @MainActor deinit async {}
}
