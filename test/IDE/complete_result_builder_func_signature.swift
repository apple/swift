// RUN: %empty-directory(%t)
// RUN: %target-swift-ide-test -batch-code-completion -source-filename %s -filecheck %raw-FileCheck -completion-output-dir %t

@resultBuilder struct MyBuilder {
    static func buildBlock() -> Int
    static func buildBlock<Content>(_ content: Content) -> Content
}

@MyBuilder func test(action: () -> #^COMPLETE^#Void) {}

// COMPLETE: Decl[TypeAlias]/OtherModule[Swift]/IsSystem: Void[#Void#]; name=Void
