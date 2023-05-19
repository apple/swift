import freestanding_macro_library

@freestanding(declaration, names: arbitrary)
public macro bitwidthNumberedStructs(_ baseName: String, blah: Bool) = #externalMacro(module: "MacroDefinition", type: "DefineBitwidthNumberedStructsMacro")
