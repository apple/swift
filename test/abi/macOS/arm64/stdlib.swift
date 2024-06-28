// RUN: %empty-directory(%t)
// RUN: %llvm-nm -g --defined-only -f just-symbols %stdlib_dir/arm64/libswiftCore.dylib > %t/symbols
// RUN: %abi-symbol-checker %s %t/symbols
// RUN: diff -u %S/../../Inputs/macOS/arm64/stdlib/baseline %t/symbols

// REQUIRES: swift_stdlib_no_asserts
// REQUIRES: STDLIB_VARIANT=macosx-arm64

// *** DO NOT DISABLE OR XFAIL THIS TEST. *** (See comment below.)

// Welcome, Build Wrangler!
//
// This file lists APIs that have recently changed in a way that potentially
// indicates an ABI- or source-breaking problem.
//
// A failure in this test indicates that there is a potential breaking change in
// the Standard Library. If you observe a failure outside of a PR test, please
// reach out to the Standard Library team directly to make sure this gets
// resolved quickly! If your own PR fails in this test, you probably have an
// ABI- or source-breaking change in your commits. Please go and fix it.
//
// Please DO NOT DISABLE THIS TEST. In addition to ignoring the current set of
// ABI breaks, XFAILing this test also silences any future ABI breaks that may
// land on this branch, which simply generates extra work for the next person
// that picks up the mess.
//
// Instead of disabling this test, you'll need to extend the list of expected
// changes at the bottom. (You'll also need to do this if your own PR triggers
// false positives, or if you have special permission to break things.) You can
// find a diff of what needs to be added in the output of the failed test run.
// The order of lines doesn't matter, and you can also include comments to refer
// to any bugs you filed.
//
// Thank you for your help ensuring the stdlib remains compatible with its past!
//                                            -- Your friendly stdlib engineers

// Standard Library Symbols

// Swift.String.init<A, B where A: Swift._UnicodeEncoding, B: Swift.Sequence, A.CodeUnit == B.Element>(validating: B, as: A.Type) -> Swift.String?
Added: _$sSS10validating2asSSSgq__xmtcs16_UnicodeEncodingRzSTR_7ElementQy_8CodeUnitRtzr0_lufC

// Swift.String.init<A, B where A: Swift._UnicodeEncoding, B: Swift.Sequence, A.CodeUnit == Swift.UInt8, B.Element == Swift.Int8>(validating: B, as: A.Type) -> Swift.String?
Added: _$sSS10validating2asSSSgq__xmtcs16_UnicodeEncodingRzSTR_s5UInt8V8CodeUnitRtzs4Int8V7ElementRt_r0_lufC

// static Swift.String._validate<A where A: Swift._UnicodeEncoding>(_: Swift.UnsafeBufferPointer<A.CodeUnit>, as: A.Type) -> Swift.String?
Added: _$sSS9_validate_2asSSSgSRy8CodeUnitQzG_xmts16_UnicodeEncodingRzlFZ

// class __StaticArrayStorage
Added: _$ss20__StaticArrayStorageC12_doNotCallMeAByt_tcfC
Added: _$ss20__StaticArrayStorageC12_doNotCallMeAByt_tcfCTj
Added: _$ss20__StaticArrayStorageC12_doNotCallMeAByt_tcfCTq
Added: _$ss20__StaticArrayStorageC12_doNotCallMeAByt_tcfc
Added: _$ss20__StaticArrayStorageC16_doNotCallMeBaseAByt_tcfC
Added: _$ss20__StaticArrayStorageC16_doNotCallMeBaseAByt_tcfc
Added: _$ss20__StaticArrayStorageC16canStoreElements13ofDynamicTypeSbypXp_tF
Added: _$ss20__StaticArrayStorageC17staticElementTypeypXpvg
Added: _$ss20__StaticArrayStorageCMa
Added: _$ss20__StaticArrayStorageCMn
Added: _$ss20__StaticArrayStorageCMo
Added: _$ss20__StaticArrayStorageCMu
Added: _$ss20__StaticArrayStorageCN
Added: _$ss20__StaticArrayStorageCfD
Added: _$ss20__StaticArrayStorageCfd
Added: _OBJC_CLASS_$__TtCs20__StaticArrayStorage
Added: _OBJC_METACLASS_$__TtCs20__StaticArrayStorage

// struct DiscontiguousSlice
Added: _$ss18DiscontiguousSliceV10startIndexAB0D0Vyx_Gvg
Added: _$ss18DiscontiguousSliceV10startIndexAB0D0Vyx_GvpMV
Added: _$ss18DiscontiguousSliceV11descriptionSSvg
Added: _$ss18DiscontiguousSliceV11descriptionSSvpMV
Added: _$ss18DiscontiguousSliceV20_failEarlyRangeCheck_6boundsyAB5IndexVyx_G_SNyAGGtF
Added: _$ss18DiscontiguousSliceV20_failEarlyRangeCheck_6boundsyAB5IndexVyx_G_SnyAGGtF
Added: _$ss18DiscontiguousSliceV20_failEarlyRangeCheck_6boundsySnyAB5IndexVyx_GG_AHtF
Added: _$ss18DiscontiguousSliceV22_copyToContiguousArrays0eF0Vy7ElementQzGyF
Added: _$ss18DiscontiguousSliceV30_customIndexOfEquatableElementyAB0D0Vyx_GSgSg0G0QzF
Added: _$ss18DiscontiguousSliceV31_customContainsEquatableElementySbSg0F0QzF
Added: _$ss18DiscontiguousSliceV34_customLastIndexOfEquatableElementyAB0E0Vyx_GSgSg0H0QzF
Added: _$ss18DiscontiguousSliceV4basexvg
Added: _$ss18DiscontiguousSliceV4basexvpMV
Added: _$ss18DiscontiguousSliceV5IndexV11descriptionSSvg
Added: _$ss18DiscontiguousSliceV5IndexV11descriptionSSvpMV
Added: _$ss18DiscontiguousSliceV5IndexV1loiySbADyx_G_AFtFZ
Added: _$ss18DiscontiguousSliceV5IndexV2eeoiySbADyx_G_AFtFZ
Added: _$ss18DiscontiguousSliceV5IndexV4baseACQzvg
Added: _$ss18DiscontiguousSliceV5IndexV4baseACQzvpMV
Added: _$ss18DiscontiguousSliceV5IndexVMa
Added: _$ss18DiscontiguousSliceV5IndexVMn
Added: _$ss18DiscontiguousSliceV5IndexVsSHACRpzrlE4hash4intoys6HasherVz_tF
Added: _$ss18DiscontiguousSliceV5IndexVsSHACRpzrlE9hashValueSivg
Added: _$ss18DiscontiguousSliceV5IndexVsSHACRpzrlE9hashValueSivpMV
Added: _$ss18DiscontiguousSliceV5IndexVyx_GSHsSHACRpzrlMc
Added: _$ss18DiscontiguousSliceV5IndexVyx_GSLsMc
Added: _$ss18DiscontiguousSliceV5IndexVyx_GSLsWP
Added: _$ss18DiscontiguousSliceV5IndexVyx_GSQsMc
Added: _$ss18DiscontiguousSliceV5IndexVyx_GSQsWP
Added: _$ss18DiscontiguousSliceV5IndexVyx_Gs23CustomStringConvertiblesMc
Added: _$ss18DiscontiguousSliceV5IndexVyx_Gs23CustomStringConvertiblesWP
Added: _$ss18DiscontiguousSliceV5_base9subrangesAByxGx_s8RangeSetVy5IndexQzGtcfC
Added: _$ss18DiscontiguousSliceV5countSivg
Added: _$ss18DiscontiguousSliceV5countSivpMV
Added: _$ss18DiscontiguousSliceV5index5afterAB5IndexVyx_GAG_tF
Added: _$ss18DiscontiguousSliceV6_index2ofAB5IndexVyx_GSgAEQz_tF
Added: _$ss18DiscontiguousSliceV7isEmptySbvg
Added: _$ss18DiscontiguousSliceV7isEmptySbvpMV
Added: _$ss18DiscontiguousSliceV8distance4from2toSiAB5IndexVyx_G_AHtF
Added: _$ss18DiscontiguousSliceV8endIndexAB0D0Vyx_Gvg
Added: _$ss18DiscontiguousSliceV8endIndexAB0D0Vyx_GvpMV
Added: _$ss18DiscontiguousSliceV9subrangess8RangeSetVy5IndexQzGvg
Added: _$ss18DiscontiguousSliceV9subrangess8RangeSetVy5IndexQzGvpMV
Added: _$ss18DiscontiguousSliceVMa
Added: _$ss18DiscontiguousSliceVMn
Added: _$ss18DiscontiguousSliceVsSH7ElementRpzrlE4hash4intoys6HasherVz_tF
Added: _$ss18DiscontiguousSliceVsSH7ElementRpzrlE9hashValueSivg
Added: _$ss18DiscontiguousSliceVsSH7ElementRpzrlE9hashValueSivpMV
Added: _$ss18DiscontiguousSliceVsSKRzrlE5index6beforeAB5IndexVyx_GAG_tF
Added: _$ss18DiscontiguousSliceVsSMRzrlEy7ElementQzAB5IndexVyx_GciM
Added: _$ss18DiscontiguousSliceVsSMRzrlEy7ElementQzAB5IndexVyx_Gcig
Added: _$ss18DiscontiguousSliceVsSMRzrlEy7ElementQzAB5IndexVyx_GcipMV
Added: _$ss18DiscontiguousSliceVsSMRzrlEy7ElementQzAB5IndexVyx_Gcis
Added: _$ss18DiscontiguousSliceVsSQ7ElementRpzrlE2eeoiySbAByxG_AFtFZ
Added: _$ss18DiscontiguousSliceVy7ElementQzAB5IndexVyx_Gcig
Added: _$ss18DiscontiguousSliceVy7ElementQzAB5IndexVyx_GcipMV
Added: _$ss18DiscontiguousSliceVyAByxGSnyAB5IndexVyx_GGcig
Added: _$ss18DiscontiguousSliceVyAByxGSnyAB5IndexVyx_GGcipMV
Added: _$ss18DiscontiguousSliceVyxGSHsSH7ElementRpzrlMc
Added: _$ss18DiscontiguousSliceVyxGSKsSKRzrlMc
Added: _$ss18DiscontiguousSliceVyxGSQsSQ7ElementRpzrlMc
Added: _$ss18DiscontiguousSliceVyxGSTsMc
Added: _$ss18DiscontiguousSliceVyxGSlsMc
Added: _$ss18DiscontiguousSliceVyxGs23CustomStringConvertiblesMc
Added: _$ss18DiscontiguousSliceVyxGs23CustomStringConvertiblesWP

// (extension in Swift):Swift.Collection.removingSubranges(Swift.RangeSet<A.Index>) -> Swift.DiscontiguousSlice<A>
Added: _$sSlsE17removingSubrangesys18DiscontiguousSliceVyxGs8RangeSetVy5IndexQzGF

// (extension in Swift):Swift.Collection.subscript.getter : (Swift.RangeSet<A.Index>) -> Swift.DiscontiguousSlice<A>
Added: _$sSlsEys18DiscontiguousSliceVyxGs8RangeSetVy5IndexQzGcig

// property descriptor for (extension in Swift):Swift.Collection.subscript(Swift.RangeSet<A.Index>) -> Swift.DiscontiguousSlice<A>
Added: _$sSlsEys18DiscontiguousSliceVyxGs8RangeSetVy5IndexQzGcipMV

// struct RangeSet
Added: _$ss8RangeSetV10isDisjointySbAByxGF
Added: _$ss8RangeSetV10isSuperset2ofSbAByxG_tF
Added: _$ss8RangeSetV11descriptionSSvg
Added: _$ss8RangeSetV11descriptionSSvpMV
Added: _$ss8RangeSetV11subtractingyAByxGADF
Added: _$ss8RangeSetV12intersectionyAByxGADF
Added: _$ss8RangeSetV14_orderedRangesAByxGSaySnyxGG_tcfC
Added: _$ss8RangeSetV14isStrictSubset2ofSbAByxG_tF
Added: _$ss8RangeSetV16_checkInvariantsyyF
Added: _$ss8RangeSetV16formIntersectionyyAByxGF
Added: _$ss8RangeSetV16isStrictSuperset2ofSbAByxG_tF
Added: _$ss8RangeSetV19symmetricDifferenceyAByxGADnF
Added: _$ss8RangeSetV23formSymmetricDifferenceyyAByxGnF
Added: _$ss8RangeSetV2eeoiySbAByxG_ADtFZ
Added: _$ss8RangeSetV5unionyAByxGADnF
Added: _$ss8RangeSetV6RangesV010_indicesOfA0_2in15includeAdjacentSnySiGSnyxG_s15ContiguousArrayVyAIGSbtF
Added: _$ss8RangeSetV6RangesV010_unorderedC0ADyx_GSaySnyxGG_tcfC
Added: _$ss8RangeSetV6RangesV10startIndexSivg
Added: _$ss8RangeSetV6RangesV10startIndexSivpMV
Added: _$ss8RangeSetV6RangesV11descriptionSSvg
Added: _$ss8RangeSetV6RangesV11descriptionSSvpMV
Added: _$ss8RangeSetV6RangesV13_intersectionyADyx_GAFF
Added: _$ss8RangeSetV6RangesV2eeoiySbADyx_G_AFtFZ
Added: _$ss8RangeSetV6RangesV5_gaps9boundedByADyx_GSnyxG_tF
Added: _$ss8RangeSetV6RangesV5countSivg
Added: _$ss8RangeSetV6RangesV5countSivpMV
Added: _$ss8RangeSetV6RangesV6_rangeADyx_GSnyxG_tcfC
Added: _$ss8RangeSetV6RangesV7_insert10contentsOfSbSnyxG_tF
Added: _$ss8RangeSetV6RangesV7_rangesADyx_GSaySnyxGG_tcfC
Added: _$ss8RangeSetV6RangesV7_remove10contentsOfySnyxG_tF
Added: _$ss8RangeSetV6RangesV8endIndexSivg
Added: _$ss8RangeSetV6RangesV8endIndexSivpMV
Added: _$ss8RangeSetV6RangesV9_containsySbxF
Added: _$ss8RangeSetV6RangesVADyx_GycfC
Added: _$ss8RangeSetV6RangesVMa
Added: _$ss8RangeSetV6RangesVMn
Added: _$ss8RangeSetV6RangesVsSHRzrlE4hash4intoys6HasherVz_tF
Added: _$ss8RangeSetV6RangesVsSHRzrlE9hashValueSivg
Added: _$ss8RangeSetV6RangesVsSHRzrlE9hashValueSivpMV
Added: _$ss8RangeSetV6RangesVySnyxGSicig
Added: _$ss8RangeSetV6RangesVySnyxGSicipMV
Added: _$ss8RangeSetV6RangesVyx_GSHsSHRzrlMc
Added: _$ss8RangeSetV6RangesVyx_GSKsMc
Added: _$ss8RangeSetV6RangesVyx_GSQsMc
Added: _$ss8RangeSetV6RangesVyx_GSQsWP
Added: _$ss8RangeSetV6RangesVyx_GSTsMc
Added: _$ss8RangeSetV6RangesVyx_GSksMc
Added: _$ss8RangeSetV6RangesVyx_GSlsMc
Added: _$ss8RangeSetV6RangesVyx_Gs23CustomStringConvertiblesMc
Added: _$ss8RangeSetV6RangesVyx_Gs23CustomStringConvertiblesWP
Added: _$ss8RangeSetV6insert_6withinSbx_qd__t5IndexQyd__RszSlRd__lF
Added: _$ss8RangeSetV6insert10contentsOfySnyxG_tF
Added: _$ss8RangeSetV6rangesAB6RangesVyx_Gvg
Added: _$ss8RangeSetV6rangesAB6RangesVyx_GvpMV
Added: _$ss8RangeSetV6remove10contentsOfySnyxG_tF
Added: _$ss8RangeSetV6remove_6withinyx_qd__t5IndexQyd__RszSlRd__lF
Added: _$ss8RangeSetV7_rangesAB6RangesVyx_GvM
Added: _$ss8RangeSetV7_rangesAB6RangesVyx_Gvg
Added: _$ss8RangeSetV7_rangesAB6RangesVyx_GvpMV
Added: _$ss8RangeSetV7_rangesAB6RangesVyx_Gvs
Added: _$ss8RangeSetV7_rangesAByxGAB6RangesVyx_G_tcfC
Added: _$ss8RangeSetV7isEmptySbvg
Added: _$ss8RangeSetV7isEmptySbvpMV
Added: _$ss8RangeSetV8containsySbxF
Added: _$ss8RangeSetV8isSubset2ofSbAByxG_tF
Added: _$ss8RangeSetV8subtractyyAByxGF
Added: _$ss8RangeSetV9_inverted6withinAByxGqd___t5IndexQyd__RszSlRd__lF
Added: _$ss8RangeSetV9formUnionyyAByxGnF
Added: _$ss8RangeSetVAByxGycfC
Added: _$ss8RangeSetVMa
Added: _$ss8RangeSetVMn
Added: _$ss8RangeSetV_6withinAByxGqd___qd_0_tc7ElementQyd__RszSTRd__SlRd_0_5IndexQyd_0_AFRSr0_lufC
Added: _$ss8RangeSetVsSHRzrlE4hash4intoys6HasherVz_tF
Added: _$ss8RangeSetVsSHRzrlE9hashValueSivg
Added: _$ss8RangeSetVsSHRzrlE9hashValueSivpMV
Added: _$ss8RangeSetVyAByxGSnyxGcfC
Added: _$ss8RangeSetVyAByxGqd__cSTRd__SnyxG7ElementRtd__lufC
Added: _$ss8RangeSetVyxGSHsSHRzrlMc
Added: _$ss8RangeSetVyxGSQsMc
Added: _$ss8RangeSetVyxGSQsWP
Added: _$ss8RangeSetVyxGs23CustomStringConvertiblesMc
Added: _$ss8RangeSetVyxGs23CustomStringConvertiblesWP

// (extension in Swift):Swift.Collection< where A.Element: Swift.Equatable>.indices(of: A.Element) -> Swift.RangeSet<A.Index>
Added: _$sSlsSQ7ElementRpzrlE7indices2ofs8RangeSetVy5IndexQzGAB_tF

// (extension in Swift):Swift.RangeReplaceableCollection.removeSubranges(Swift.RangeSet<A.Index>) -> ()
Added: _$sSmsE15removeSubrangesyys8RangeSetVy5IndexQzGF

// (extension in Swift):Swift.Collection.indices(where: (A.Element) throws -> Swift.Bool) throws -> Swift.RangeSet<A.Index>
Added: _$sSlsE7indices5wheres8RangeSetVy5IndexQzGSb7ElementQzKXE_tKF

// (extension in Swift):Swift.MutableCollection< where A: Swift.RangeReplaceableCollection>.removeSubranges(Swift.RangeSet<A.Swift.Collection.Index>) -> ()
Added: _$sSMsSmRzrlE15removeSubrangesyys8RangeSetVy5IndexSlQzGF

// (extension in Swift):Swift.MutableCollection.moveSubranges(_: Swift.RangeSet<A.Index>, to: A.Index) -> Swift.Range<A.Index>
Added: _$sSMsE13moveSubranges_2toSny5IndexQzGs8RangeSetVyADG_ADtF

// `isEmpty` is a newly explicit property on Unsafe[Mutable]BufferPointer
// property descriptor for Swift.UnsafeBufferPointer.isEmpty : Swift.Bool
Added: _$sSR7isEmptySbvpMV
// property descriptor for Swift.UnsafeMutableBufferPointer.isEmpty : Swift.Bool
Added: _$sSr7isEmptySbvpMV

// Int128
Added: _$sSYsSERzs6Int128V8RawValueSYRtzrlE6encode2toys7Encoder_p_tKF
Added: _$sSYsSERzs7UInt128V8RawValueSYRtzrlE6encode2toys7Encoder_p_tKF
Added: _$sSYsSeRzs6Int128V8RawValueSYRtzrlE4fromxs7Decoder_p_tKcfC
Added: _$ss22KeyedDecodingContainerV15decodeIfPresent_6forKeys6Int128VSgAFm_xtKF
Added: _$ss22KeyedDecodingContainerV6decode_6forKeys6Int128VAFm_xtKF
Added: _$ss22KeyedEncodingContainerV15encodeIfPresent_6forKeyys6Int128VSg_xtKF
Added: _$ss22KeyedEncodingContainerV6encode_6forKeyys6Int128V_xtKF
Added: _$ss24UnkeyedDecodingContainerP15decodeIfPresentys6Int128VSgAEmKFTj
Added: _$ss24UnkeyedDecodingContainerP15decodeIfPresentys6Int128VSgAEmKFTq
Added: _$ss24UnkeyedDecodingContainerP6decodeys6Int128VAEmKFTj
Added: _$ss24UnkeyedDecodingContainerP6decodeys6Int128VAEmKFTq
Added: _$ss24UnkeyedDecodingContainerPsE15decodeIfPresentys6Int128VSgAEmKF
Added: _$ss24UnkeyedDecodingContainerPsE6decodeys6Int128VAEmKF
Added: _$ss24UnkeyedEncodingContainerP6encode10contentsOfyqd___tKSTRd__s6Int128V7ElementRtd__lFTj
Added: _$ss24UnkeyedEncodingContainerP6encode10contentsOfyqd___tKSTRd__s6Int128V7ElementRtd__lFTq
Added: _$ss24UnkeyedEncodingContainerP6encodeyys6Int128VKFTj
Added: _$ss24UnkeyedEncodingContainerP6encodeyys6Int128VKFTq
Added: _$ss24UnkeyedEncodingContainerPsE6encode10contentsOfyqd___tKSTRd__s6Int128V7ElementRtd__lF
Added: _$ss24UnkeyedEncodingContainerPsE6encodeyys6Int128VKF
Added: _$ss28SingleValueDecodingContainerP6decodeys6Int128VAEmKFTj
Added: _$ss28SingleValueDecodingContainerP6decodeys6Int128VAEmKFTq
Added: _$ss28SingleValueDecodingContainerPsE6decodeys6Int128VAEmKF
Added: _$ss28SingleValueDecodingContainerPss07UnkeyedcD0RzrlE6decodeys6Int128VAFmKF
Added: _$ss28SingleValueEncodingContainerP6encodeyys6Int128VKFTj
Added: _$ss28SingleValueEncodingContainerP6encodeyys6Int128VKFTq
Added: _$ss28SingleValueEncodingContainerPsE6encodeyys6Int128VKF
Added: _$ss28SingleValueEncodingContainerPss07UnkeyedcD0RzrlE6encodeyys6Int128VKF
Added: _$ss30KeyedDecodingContainerProtocolP15decodeIfPresent_6forKeys6Int128VSgAFm_0I0QztKFTj
Added: _$ss30KeyedDecodingContainerProtocolP15decodeIfPresent_6forKeys6Int128VSgAFm_0I0QztKFTq
Added: _$ss30KeyedDecodingContainerProtocolP6decode_6forKeys6Int128VAFm_0G0QztKFTj
Added: _$ss30KeyedDecodingContainerProtocolP6decode_6forKeys6Int128VAFm_0G0QztKFTq
Added: _$ss30KeyedDecodingContainerProtocolPsE15decodeIfPresent_6forKeys6Int128VSgAFm_0I0QztKF
Added: _$ss30KeyedDecodingContainerProtocolPsE6decode_6forKeys6Int128VAFm_0G0QztKF
Added: _$ss30KeyedEncodingContainerProtocolP15encodeIfPresent_6forKeyys6Int128VSg_0I0QztKFTj
Added: _$ss30KeyedEncodingContainerProtocolP15encodeIfPresent_6forKeyys6Int128VSg_0I0QztKFTq
Added: _$ss30KeyedEncodingContainerProtocolP6encode_6forKeyys6Int128V_0G0QztKFTj
Added: _$ss30KeyedEncodingContainerProtocolP6encode_6forKeyys6Int128V_0G0QztKFTq
Added: _$ss30KeyedEncodingContainerProtocolPsE15encodeIfPresent_6forKeyys6Int128VSg_0I0QztKF
Added: _$ss30KeyedEncodingContainerProtocolPsE6encode_6forKeyys6Int128V_0G0QztKF
Added: _$ss6Int128V10bitPatternABs7UInt128V_tcfC
Added: _$ss6Int128V11byteSwappedABvg
Added: _$ss6Int128V11byteSwappedABvpMV
Added: _$ss6Int128V12customMirrors0C0Vvg
Added: _$ss6Int128V12customMirrors0C0VvpMV
Added: _$ss6Int128V15_truncatingBitsABSu_tcfC
Added: _$ss6Int128V15nonzeroBitCountSivg
Added: _$ss6Int128V15nonzeroBitCountSivpMV
Added: _$ss6Int128V18truncatingIfNeededABx_tcSzRzlufC
Added: _$ss6Int128V19leadingZeroBitCountSivg
Added: _$ss6Int128V19leadingZeroBitCountSivpMV
Added: _$ss6Int128V1doiyA2B_ABtFZ
Added: _$ss6Int128V1loiySbAB_ABtFZ
Added: _$ss6Int128V1moiyA2B_ABtFZ
Added: _$ss6Int128V1poiyA2B_ABtFZ
Added: _$ss6Int128V1roiyA2B_ABtFZ
Added: _$ss6Int128V1soiyA2B_ABtFZ
Added: _$ss6Int128V20trailingZeroBitCountSivg
Added: _$ss6Int128V20trailingZeroBitCountSivpMV
Added: _$ss6Int128V22_builtinIntegerLiteralABBI_tcfC
Added: _$ss6Int128V23addingReportingOverflowyAB12partialValue_Sb8overflowtABF
Added: _$ss6Int128V24dividedReportingOverflow2byAB12partialValue_Sb8overflowtAB_tF
Added: _$ss6Int128V26remainderReportingOverflow10dividingByAB12partialValue_Sb8overflowtAB_tF
Added: _$ss6Int128V27multipliedReportingOverflow2byAB12partialValue_Sb8overflowtAB_tF
Added: _$ss6Int128V28subtractingReportingOverflowyAB12partialValue_Sb8overflowtABF
Added: _$ss6Int128V2aeoiyyABz_ABtFZ
Added: _$ss6Int128V2amoiyA2B_ABtFZ
Added: _$ss6Int128V2deoiyyABz_ABtFZ
Added: _$ss6Int128V2eeoiySbAB_ABtFZ
Added: _$ss6Int128V2meoiyyABz_ABtFZ
Added: _$ss6Int128V2oeoiyyABz_ABtFZ
Added: _$ss6Int128V2reoiyyABz_ABtFZ
Added: _$ss6Int128V2xeoiyyABz_ABtFZ
Added: _$ss6Int128V4_low5_highABs6UInt64V_s5Int64VtcfC
Added: _$ss6Int128V4_lows6UInt64Vvg
Added: _$ss6Int128V4_lows6UInt64VvpMV
Added: _$ss6Int128V3maxABvgZ
Added: _$ss6Int128V3minABvgZ
Added: _$ss6Int128V4aggeoiyyABz_ABtFZ
Added: _$ss6Int128V4alleoiyyABz_ABtFZ
Added: _$ss6Int128V4fromABs7Decoder_p_tKcfC
Added: _$ss6Int128V4hash4intoys6HasherVz_tF
Added: _$ss6Int128V5_highs5Int64Vvg
Added: _$ss6Int128V5_highs5Int64VvpMV
Added: _$ss6Int128V4zeroABvgZ
Added: _$ss6Int128V5wordss7UInt128V5WordsVvg
Added: _$ss6Int128V5wordss7UInt128V5WordsVvpMV
Added: _$ss6Int128V6_valueBi128_vM
Added: _$ss6Int128V6_valueBi128_vg
Added: _$ss6Int128V6_valueBi128_vpMV
Added: _$ss6Int128V6_valueBi128_vs
Added: _$ss6Int128V6encode2toys7Encoder_p_tKF
Added: _$ss6Int128V7exactlyABSgx_tcSBRzlufC
Added: _$ss6Int128V7exactlyABSgx_tcSzRzlufC
Added: _$ss6Int128V8bitWidthSivgZ
Added: _$ss6Int128V8clampingABx_tcSzRzlufC
Added: _$ss6Int128V9hashValueSivg
Added: _$ss6Int128V9hashValueSivpMV
Added: _$ss6Int128V9magnitudes7UInt128Vvg
Added: _$ss6Int128V9magnitudes7UInt128VvpMV
Added: _$ss6Int128VMa
Added: _$ss6Int128VMn
Added: _$ss6Int128VN
Added: _$ss6Int128VSEsMc
Added: _$ss6Int128VSEsWP
Added: _$ss6Int128VSHsMc
Added: _$ss6Int128VSHsWP
Added: _$ss6Int128VSLsMc
Added: _$ss6Int128VSLsWP
Added: _$ss6Int128VSQsMc
Added: _$ss6Int128VSQsWP
Added: _$ss6Int128VSZsMc
Added: _$ss6Int128VSZsWP
Added: _$ss6Int128VSesMc
Added: _$ss6Int128VSesWP
Added: _$ss6Int128VSjsMc
Added: _$ss6Int128VSjsWP
Added: _$ss6Int128VSxsMc
Added: _$ss6Int128VSxsWP
Added: _$ss6Int128VSzsMc
Added: _$ss6Int128VSzsWP
Added: _$ss6Int128Vs13SignedNumericsMc
Added: _$ss6Int128Vs13SignedNumericsWP
Added: _$ss6Int128Vs17CustomReflectablesMc
Added: _$ss6Int128Vs17CustomReflectablesWP
Added: _$ss6Int128Vs17FixedWidthIntegersMc
Added: _$ss6Int128Vs17FixedWidthIntegersWP
Added: _$ss6Int128Vs18AdditiveArithmeticsMc
Added: _$ss6Int128Vs18AdditiveArithmeticsWP
Added: _$ss6Int128Vs23CustomStringConvertiblesMc
Added: _$ss6Int128Vs23CustomStringConvertiblesWP
Added: _$ss6Int128Vs25LosslessStringConvertiblesMc
Added: _$ss6Int128Vs25LosslessStringConvertiblesWP
Added: _$ss6Int128Vs27ExpressibleByIntegerLiteralsMc
Added: _$ss6Int128Vs27ExpressibleByIntegerLiteralsWP
Added: _$ss6Int128Vs35_ExpressibleByBuiltinIntegerLiteralsMc
Added: _$ss6Int128Vs35_ExpressibleByBuiltinIntegerLiteralsWP
Added: _$ss6Int128VyABBi128_cfC
Added: _$ss6Int128VyABxcSBRzlufC
Added: _$ss6Int128VyABxcSzRzlufC
Added: _$ss6Int128V8_lowWordSuvg
Added: _$ss6Int128V8_lowWordSuvpMV

// UInt128
Added: _$sSYsSeRzs7UInt128V8RawValueSYRtzrlE4fromxs7Decoder_p_tKcfC
Added: _$ss22KeyedDecodingContainerV15decodeIfPresent_6forKeys7UInt128VSgAFm_xtKF
Added: _$ss22KeyedDecodingContainerV6decode_6forKeys7UInt128VAFm_xtKF
Added: _$ss22KeyedEncodingContainerV15encodeIfPresent_6forKeyys7UInt128VSg_xtKF
Added: _$ss22KeyedEncodingContainerV6encode_6forKeyys7UInt128V_xtKF
Added: _$ss24UnkeyedDecodingContainerP15decodeIfPresentys7UInt128VSgAEmKFTj
Added: _$ss24UnkeyedDecodingContainerP15decodeIfPresentys7UInt128VSgAEmKFTq
Added: _$ss24UnkeyedDecodingContainerP6decodeys7UInt128VAEmKFTj
Added: _$ss24UnkeyedDecodingContainerP6decodeys7UInt128VAEmKFTq
Added: _$ss24UnkeyedDecodingContainerPsE15decodeIfPresentys7UInt128VSgAEmKF
Added: _$ss24UnkeyedDecodingContainerPsE6decodeys7UInt128VAEmKF
Added: _$ss24UnkeyedEncodingContainerP6encode10contentsOfyqd___tKSTRd__s7UInt128V7ElementRtd__lFTj
Added: _$ss24UnkeyedEncodingContainerP6encode10contentsOfyqd___tKSTRd__s7UInt128V7ElementRtd__lFTq
Added: _$ss24UnkeyedEncodingContainerP6encodeyys7UInt128VKFTj
Added: _$ss24UnkeyedEncodingContainerP6encodeyys7UInt128VKFTq
Added: _$ss24UnkeyedEncodingContainerPsE6encode10contentsOfyqd___tKSTRd__s7UInt128V7ElementRtd__lF
Added: _$ss24UnkeyedEncodingContainerPsE6encodeyys7UInt128VKF
Added: _$ss28SingleValueDecodingContainerP6decodeys7UInt128VAEmKFTj
Added: _$ss28SingleValueDecodingContainerP6decodeys7UInt128VAEmKFTq
Added: _$ss28SingleValueDecodingContainerPsE6decodeys7UInt128VAEmKF
Added: _$ss28SingleValueDecodingContainerPss07UnkeyedcD0RzrlE6decodeys7UInt128VAFmKF
Added: _$ss28SingleValueEncodingContainerP6encodeyys7UInt128VKFTj
Added: _$ss28SingleValueEncodingContainerP6encodeyys7UInt128VKFTq
Added: _$ss28SingleValueEncodingContainerPsE6encodeyys7UInt128VKF
Added: _$ss28SingleValueEncodingContainerPss07UnkeyedcD0RzrlE6encodeyys7UInt128VKF
Added: _$ss30KeyedDecodingContainerProtocolP15decodeIfPresent_6forKeys7UInt128VSgAFm_0I0QztKFTj
Added: _$ss30KeyedDecodingContainerProtocolP15decodeIfPresent_6forKeys7UInt128VSgAFm_0I0QztKFTq
Added: _$ss30KeyedDecodingContainerProtocolP6decode_6forKeys7UInt128VAFm_0G0QztKFTj
Added: _$ss30KeyedDecodingContainerProtocolP6decode_6forKeys7UInt128VAFm_0G0QztKFTq
Added: _$ss30KeyedDecodingContainerProtocolPsE15decodeIfPresent_6forKeys7UInt128VSgAFm_0I0QztKF
Added: _$ss30KeyedDecodingContainerProtocolPsE6decode_6forKeys7UInt128VAFm_0G0QztKF
Added: _$ss30KeyedEncodingContainerProtocolP15encodeIfPresent_6forKeyys7UInt128VSg_0I0QztKFTj
Added: _$ss30KeyedEncodingContainerProtocolP15encodeIfPresent_6forKeyys7UInt128VSg_0I0QztKFTq
Added: _$ss30KeyedEncodingContainerProtocolP6encode_6forKeyys7UInt128V_0G0QztKFTj
Added: _$ss30KeyedEncodingContainerProtocolP6encode_6forKeyys7UInt128V_0G0QztKFTq
Added: _$ss30KeyedEncodingContainerProtocolPsE15encodeIfPresent_6forKeyys7UInt128VSg_0I0QztKF
Added: _$ss30KeyedEncodingContainerProtocolPsE6encode_6forKeyys7UInt128V_0G0QztKF
Added: _$ss7UInt128V10bitPatternABs6Int128V_tcfC
Added: _$ss7UInt128V11byteSwappedABvg
Added: _$ss7UInt128V11byteSwappedABvpMV
Added: _$ss7UInt128V12customMirrors0C0Vvg
Added: _$ss7UInt128V12customMirrors0C0VvpMV
Added: _$ss7UInt128V15_truncatingBitsABSu_tcfC
Added: _$ss7UInt128V15nonzeroBitCountSivg
Added: _$ss7UInt128V15nonzeroBitCountSivpMV
Added: _$ss7UInt128V18truncatingIfNeededABx_tcSzRzlufC
Added: _$ss7UInt128V19leadingZeroBitCountSivg
Added: _$ss7UInt128V19leadingZeroBitCountSivpMV
Added: _$ss7UInt128V1doiyA2B_ABtFZ
Added: _$ss7UInt128V1loiySbAB_ABtFZ
Added: _$ss7UInt128V1moiyA2B_ABtFZ
Added: _$ss7UInt128V1poiyA2B_ABtFZ
Added: _$ss7UInt128V1roiyA2B_ABtFZ
Added: _$ss7UInt128V1soiyA2B_ABtFZ
Added: _$ss7UInt128V20trailingZeroBitCountSivg
Added: _$ss7UInt128V20trailingZeroBitCountSivpMV
Added: _$ss7UInt128V22_builtinIntegerLiteralABBI_tcfC
Added: _$ss7UInt128V23addingReportingOverflowyAB12partialValue_Sb8overflowtABF
Added: _$ss7UInt128V24dividedReportingOverflow2byAB12partialValue_Sb8overflowtAB_tF
Added: _$ss7UInt128V26remainderReportingOverflow10dividingByAB12partialValue_Sb8overflowtAB_tF
Added: _$ss7UInt128V27multipliedReportingOverflow2byAB12partialValue_Sb8overflowtAB_tF
Added: _$ss7UInt128V28subtractingReportingOverflowyAB12partialValue_Sb8overflowtABF
Added: _$ss7UInt128V2aeoiyyABz_ABtFZ
Added: _$ss7UInt128V2deoiyyABz_ABtFZ
Added: _$ss7UInt128V2eeoiySbAB_ABtFZ
Added: _$ss7UInt128V2meoiyyABz_ABtFZ
Added: _$ss7UInt128V2oeoiyyABz_ABtFZ
Added: _$ss7UInt128V2reoiyyABz_ABtFZ
Added: _$ss7UInt128V2xeoiyyABz_ABtFZ
Added: _$ss7UInt128V4_low5_highABs6UInt64V_AFtcfC
Added: _$ss7UInt128V4_lows6UInt64Vvg
Added: _$ss7UInt128V4_lows6UInt64VvpMV
Added: _$ss7UInt128V3maxABvgZ
Added: _$ss7UInt128V3minABvgZ
Added: _$ss7UInt128V4aggeoiyyABz_ABtFZ
Added: _$ss7UInt128V4alleoiyyABz_ABtFZ
Added: _$ss7UInt128V4fromABs7Decoder_p_tKcfC
Added: _$ss7UInt128V4hash4intoys6HasherVz_tF
Added: _$ss7UInt128V5_highs6UInt64Vvg
Added: _$ss7UInt128V5_highs6UInt64VvpMV
Added: _$ss7UInt128V4zeroABvgZ
Added: _$ss7UInt128V5WordsV10startIndexSivg
Added: _$ss7UInt128V5WordsV10startIndexSivpMV
Added: _$ss7UInt128V5WordsV5countSivg
Added: _$ss7UInt128V5WordsV5countSivpMV
Added: _$ss7UInt128V5WordsV5index5afterS2i_tF
Added: _$ss7UInt128V5WordsV5index6beforeS2i_tF
Added: _$ss7UInt128V5WordsV6_valueABvg
Added: _$ss7UInt128V5WordsV6_valueABvpMV
Added: _$ss7UInt128V5WordsV6_valueAdB_tcfC
Added: _$ss7UInt128V5WordsV7indicesSnySiGvg
Added: _$ss7UInt128V5WordsV7indicesSnySiGvpMV
Added: _$ss7UInt128V5WordsV8endIndexSivg
Added: _$ss7UInt128V5WordsV8endIndexSivpMV
Added: _$ss7UInt128V5WordsVMa
Added: _$ss7UInt128V5WordsVMn
Added: _$ss7UInt128V5WordsVN
Added: _$ss7UInt128V5WordsVSKsMc
Added: _$ss7UInt128V5WordsVSKsWP
Added: _$ss7UInt128V5WordsVSTsMc
Added: _$ss7UInt128V5WordsVSTsWP
Added: _$ss7UInt128V5WordsVSksMc
Added: _$ss7UInt128V5WordsVSksWP
Added: _$ss7UInt128V5WordsVSlsMc
Added: _$ss7UInt128V5WordsVSlsWP
Added: _$ss7UInt128V5WordsVySuSicig
Added: _$ss7UInt128V5WordsVySuSicipMV
Added: _$ss7UInt128V5wordsAB5WordsVvg
Added: _$ss7UInt128V5wordsAB5WordsVvpMV
Added: _$ss7UInt128V6_valueBi128_vM
Added: _$ss7UInt128V6_valueBi128_vg
Added: _$ss7UInt128V6_valueBi128_vpMV
Added: _$ss7UInt128V6_valueBi128_vs
Added: _$ss7UInt128V6encode2toys7Encoder_p_tKF
Added: _$ss7UInt128V7exactlyABSgx_tcSBRzlufC
Added: _$ss7UInt128V7exactlyABSgx_tcSzRzlufC
Added: _$ss7UInt128V8bitWidthSivgZ
Added: _$ss7UInt128V8clampingABx_tcSzRzlufC
Added: _$ss7UInt128V9hashValueSivg
Added: _$ss7UInt128V9hashValueSivpMV
Added: _$ss7UInt128V9magnitudeABvg
Added: _$ss7UInt128V9magnitudeABvpMV
Added: _$ss7UInt128VMa
Added: _$ss7UInt128VMn
Added: _$ss7UInt128VN
Added: _$ss7UInt128VSEsMc
Added: _$ss7UInt128VSEsWP
Added: _$ss7UInt128VSHsMc
Added: _$ss7UInt128VSHsWP
Added: _$ss7UInt128VSLsMc
Added: _$ss7UInt128VSLsWP
Added: _$ss7UInt128VSQsMc
Added: _$ss7UInt128VSQsWP
Added: _$ss7UInt128VSUsMc
Added: _$ss7UInt128VSUsWP
Added: _$ss7UInt128VSesMc
Added: _$ss7UInt128VSesWP
Added: _$ss7UInt128VSjsMc
Added: _$ss7UInt128VSjsWP
Added: _$ss7UInt128VSxsMc
Added: _$ss7UInt128VSxsWP
Added: _$ss7UInt128VSzsMc
Added: _$ss7UInt128VSzsWP
Added: _$ss7UInt128Vs17CustomReflectablesMc
Added: _$ss7UInt128Vs17CustomReflectablesWP
Added: _$ss7UInt128Vs17FixedWidthIntegersMc
Added: _$ss7UInt128Vs17FixedWidthIntegersWP
Added: _$ss7UInt128Vs18AdditiveArithmeticsMc
Added: _$ss7UInt128Vs18AdditiveArithmeticsWP
Added: _$ss7UInt128Vs23CustomStringConvertiblesMc
Added: _$ss7UInt128Vs23CustomStringConvertiblesWP
Added: _$ss7UInt128Vs25LosslessStringConvertiblesMc
Added: _$ss7UInt128Vs25LosslessStringConvertiblesWP
Added: _$ss7UInt128Vs27ExpressibleByIntegerLiteralsMc
Added: _$ss7UInt128Vs27ExpressibleByIntegerLiteralsWP
Added: _$ss7UInt128Vs35_ExpressibleByBuiltinIntegerLiteralsMc
Added: _$ss7UInt128Vs35_ExpressibleByBuiltinIntegerLiteralsWP
Added: _$ss7UInt128VyABBi128_cfC
Added: _$ss7UInt128VyABxcSBRzlufC
Added: _$ss7UInt128VyABxcSzRzlufC
Added: _$ss7UInt128V8_lowWordSuvg
Added: _$ss7UInt128V8_lowWordSuvpMV

// Fixed-width integer &* customization point
Added: _$ss17FixedWidthIntegerP2amoiyxx_xtFZTj
Added: _$ss17FixedWidthIntegerP2amoiyxx_xtFZTq

// Runtime Symbols
Added: __swift_pod_copy
Added: __swift_pod_destroy
Added: __swift_pod_direct_initializeBufferWithCopyOfBuffer
Added: __swift_pod_indirect_initializeBufferWithCopyOfBuffer
Added: __swift_validatePrespecializedMetadata
Added: __swift_exceptionPersonality
Added: _swift_willThrowTypedImpl
Added: __swift_willThrowTypedImpl
Added: __swift_enableSwizzlingOfAllocationAndRefCountingFunctions_forInstrumentsOnly
Added: _swift_clearSensitive
Added: _swift_updatePureObjCClassMetadata

// Runtime bincompat functions for Concurrency runtime to detect legacy mode
Added: _swift_bincompat_useLegacyNonCrashingExecutorChecks

// Add add SWIFT_IS_CURRENT_EXECUTOR_LEGACY_MODE_OVERRIDE
Added: _concurrencyIsCurrentExecutorLegacyModeOverride