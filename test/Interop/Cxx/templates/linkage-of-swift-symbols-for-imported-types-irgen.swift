// RUN: %cxx-all-targets(-I %S/Inputs %s -emit-ir | %FileCheck %s)

import LinkageOfSwiftSymbolsForImportedTypes

public func forceValueWitnessTableCreation() -> Any {
  let magicNumber = MagicNumber()
  return WrappedMagicNumber(t: magicNumber)
}

public func getMagicNumberForLinkageComparison() -> Any {
	return MagicNumber()
}

// CHECK: $sSo11MagicNumberVWV" = linkonce_odr hidden constant
// CHECK: $sSo11MagicNumberVMn" = linkonce_odr hidden constant
// CHECK: $sSo11MagicNumberVMf" = linkonce_odr hidden constant
// CHECK: $sSo11MagicNumberVML" = linkonce_odr hidden global

// CHECK: $sSo034__CxxTemplateInst12MagicWrapperI11D7NumberEVWV" = linkonce_odr hidden constant
// CHECK: $sSo034__CxxTemplateInst12MagicWrapperI11D7NumberEVMn" = linkonce_odr hidden constant
// CHECK: $sSo034__CxxTemplateInst12MagicWrapperI11D7NumberEVMf" = linkonce_odr hidden constant
// CHECK: $sSo034__CxxTemplateInst12MagicWrapperI11D7NumberEVML" = linkonce_odr hidden global

// CHECK: $sSo11MagicNumberVMB" = linkonce_odr hidden constant
// CHECK: $sSo11MagicNumberVMF" = linkonce_odr hidden constant

// CHECK: $sSo034__CxxTemplateInst12MagicWrapperI11D7NumberEVMB" = linkonce_odr hidden constant
// CHECK: $sSo034__CxxTemplateInst12MagicWrapperI11D7NumberEVMF" = linkonce_odr hidden constant
