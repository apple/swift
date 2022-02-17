// RUN: %empty-directory(%t)
// RUN: %target-build-swift -lswiftSwiftReflectionTest %s -o %t/reflect_Enum_values
// RUN: %target-codesign %t/reflect_Enum_values

// RUN: %target-run %target-swift-reflection-test %t/reflect_Enum_values | tee /dev/stderr | %FileCheck %s --check-prefix=CHECK%target-ptrsize --check-prefix=CHECKALL --dump-input=fail %add_num_extra_inhabitants

// REQUIRES: objc_interop
// REQUIRES: executable_test
// UNSUPPORTED: use_os_stdlib

import SwiftReflectionTest

enum OneCaseNoPayload {
  case only
}

class OneCaseNoPayloadC {
  var x = OneCaseNoPayload.only
  var y = 42
}
reflect(object: OneCaseNoPayloadC())
// CHECKALL: Reflecting an object.
// CHECKALL-NEXT: Instance pointer in child address space: 0x{{[0-9a-fA-F]+}}
// CHECKALL-NEXT: Type reference:
// CHECKALL-NEXT: (class reflect_Enum_values.OneCaseNoPayloadC)

// CHECKALL: Type info:
// CHECK64-NEXT: (class_instance size=24 alignment=8 stride=24 num_extra_inhabitants=0 bitwise_takable=1
// CHECK64-NEXT:   (field name=x offset=16
// CHECK64-NEXT:     (no_payload_enum size=0 alignment=1 stride=1 num_extra_inhabitants=0 bitwise_takable=1
// CHECK64-NEXT:       (case name=only index=0)))
// CHECK64-NEXT:   (field name=y offset=16
// CHECK64-NEXT:     (struct size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1
// CHECK64-NEXT:       (field name=_value offset=0
// CHECK64-NEXT:         (builtin size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1)))))

// CHECK32-NEXT: (class_instance size=12 alignment=4 stride=12 num_extra_inhabitants=0 bitwise_takable=1
// CHECK32-NEXT:   (field name=x offset=8
// CHECK32-NEXT:     (no_payload_enum size=0 alignment=1 stride=1 num_extra_inhabitants=0 bitwise_takable=1
// CHECK32-NEXT:       (case name=only index=0)))
// CHECK32-NEXT:   (field name=y offset=8
// CHECK32-NEXT:     (struct size=4 alignment=4 stride=4 num_extra_inhabitants=0 bitwise_takable=1
// CHECK32-NEXT:       (field name=_value offset=0
// CHECK32-NEXT:         (builtin size=4 alignment=4 stride=4 num_extra_inhabitants=0 bitwise_takable=1)))))

enum ManyCasesNoPayload {
  case a, b, c, d
}

class ManyCasesNoPayloadC {
  var a = ManyCasesNoPayload.a
  var b = ManyCasesNoPayload.b
  var c = ManyCasesNoPayload.c
  var d = ManyCasesNoPayload.d
  var s = "beeep"
}
reflect(object: ManyCasesNoPayloadC())

// CHECKALL: Reflecting an object.
// CHECKALL-NEXT: Instance pointer in child address space: 0x{{[0-9a-fA-F]+}}
// CHECKALL-NEXT: Type reference:
// CHECKALL-NEXT: (class reflect_Enum_values.ManyCasesNoPayloadC)

// CHECKALL: Type info:
// CHECK64-NEXT: (class_instance size=40 alignment=8 stride=40 num_extra_inhabitants=0 bitwise_takable=1
// CHECK64-NEXT:   (field name=a offset=16
// CHECK32-NEXT: (class_instance size=24 alignment=4 stride=24 num_extra_inhabitants=0 bitwise_takable=1
// CHECK32-NEXT:   (field name=a offset=8
// CHECKALL-NEXT:     (no_payload_enum size=1 alignment=1 stride=1 num_extra_inhabitants=252 bitwise_takable=1
// CHECKALL-NEXT:       (case name=a index=0)
// CHECKALL-NEXT:       (case name=b index=1)
// CHECKALL-NEXT:       (case name=c index=2)
// CHECKALL-NEXT:       (case name=d index=3)))
// CHECK64-NEXT:   (field name=b offset=17
// CHECK32-NEXT:   (field name=b offset=9
// CHECKALL-NEXT:     (no_payload_enum size=1 alignment=1 stride=1 num_extra_inhabitants=252 bitwise_takable=1
// CHECKALL-NEXT:       (case name=a index=0)
// CHECKALL-NEXT:       (case name=b index=1)
// CHECKALL-NEXT:       (case name=c index=2)
// CHECKALL-NEXT:       (case name=d index=3)))
// CHECK64-NEXT:   (field name=c offset=18
// CHECK32-NEXT:   (field name=c offset=10
// CHECKALL-NEXT:     (no_payload_enum size=1 alignment=1 stride=1 num_extra_inhabitants=252 bitwise_takable=1
// CHECKALL-NEXT:       (case name=a index=0)
// CHECKALL-NEXT:       (case name=b index=1)
// CHECKALL-NEXT:       (case name=c index=2)
// CHECKALL-NEXT:       (case name=d index=3)))
// CHECK64-NEXT:   (field name=d offset=19
// CHECK32-NEXT:   (field name=d offset=11
// CHECKALL-NEXT:     (no_payload_enum size=1 alignment=1 stride=1 num_extra_inhabitants=252 bitwise_takable=1
// CHECKALL-NEXT:       (case name=a index=0)
// CHECKALL-NEXT:       (case name=b index=1)
// CHECKALL-NEXT:       (case name=c index=2)
// CHECKALL-NEXT:       (case name=d index=3)))
// CHECK64-NEXT:   (field name=s offset=24
// CHECK64-NEXT:     (struct size=16 alignment=8 stride=16 num_extra_inhabitants=[[#num_extra_inhabitants_64bit]] bitwise_takable=1
// CHECK64-NEXT:       (field name=_guts offset=0
// CHECK64-NEXT:         (struct size=16 alignment=8 stride=16 num_extra_inhabitants=[[#num_extra_inhabitants_64bit]] bitwise_takable=1
// CHECK64-NEXT:           (field name=_object offset=0
// CHECK64-NEXT:             (struct size=16 alignment=8 stride=16 num_extra_inhabitants=[[#num_extra_inhabitants_64bit]] bitwise_takable=1
// CHECK64-NEXT:               (field name=_countAndFlagsBits offset=0
// CHECK64-NEXT:                 (struct size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1
// CHECK64-NEXT:                   (field name=_value offset=0
// CHECK64-NEXT:                     (builtin size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1))))
// CHECK64-NEXT:               (field name=_object offset=8
// CHECK64-NEXT:                 (builtin size=8 alignment=8 stride=8 num_extra_inhabitants=[[#num_extra_inhabitants_64bit]] bitwise_takable=1)))))))))
// CHECK32-NEXT:   (field name=s offset=12
// CHECK32-NEXT:     (struct size=12 alignment=4 stride=12 num_extra_inhabitants=253 bitwise_takable=1
// CHECK32-NEXT:       (field name=_guts offset=0
// CHECK32-NEXT:         (struct size=12 alignment=4 stride=12 num_extra_inhabitants=253 bitwise_takable=1
// CHECK32-NEXT:           (field name=_object offset=0
// CHECK32-NEXT:             (struct size=12 alignment=4 stride=12 num_extra_inhabitants=253 bitwise_takable=1
// CHECK32-NEXT:               (field name=_count offset=0
// CHECK32-NEXT:                 (struct size=4 alignment=4 stride=4 num_extra_inhabitants=0 bitwise_takable=1
// CHECK32-NEXT:                   (field name=_value offset=0
// CHECK32-NEXT:                     (builtin size=4 alignment=4 stride=4 num_extra_inhabitants=0 bitwise_takable=1))))
// CHECK32-NEXT:               (field name=_variant offset=4
// CHECK32-NEXT:                 (multi_payload_enum size=5 alignment=4 stride=8 num_extra_inhabitants=253 bitwise_takable=1
// CHECK32-NEXT:                   (case name=immortal index=0 offset=0
// CHECK32-NEXT:                     (struct size=4 alignment=4 stride=4 num_extra_inhabitants=0 bitwise_takable=1
// CHECK32-NEXT:                       (field name=_value offset=0
// CHECK32-NEXT:                         (builtin size=4 alignment=4 stride=4 num_extra_inhabitants=0 bitwise_takable=1))))
// CHECK32-NEXT:                   (case name=native index=1 offset=0
// CHECK32-NEXT:                     (class_existential size=4 alignment=4 stride=4 num_extra_inhabitants=4096 bitwise_takable=1
// CHECK32-NEXT:                       (field name=object offset=0
// CHECK32-NEXT:                         (reference kind=strong refcounting=unknown))))
// CHECK32-NEXT:                   (case name=bridged index=2 offset=0
// CHECK32-NEXT:                     (class_existential size=4 alignment=4 stride=4 num_extra_inhabitants=4096 bitwise_takable=1
// CHECK32-NEXT:                       (field name=object offset=0
// CHECK32-NEXT:                         (reference kind=strong refcounting=unknown))))))
// CHECK32-NEXT:               (field name=_discriminator offset=9
// CHECK32-NEXT:                 (struct size=1 alignment=1 stride=1 num_extra_inhabitants=0 bitwise_takable=1
// CHECK32-NEXT:                   (field name=_value offset=0
// CHECK32-NEXT:                     (builtin size=1 alignment=1 stride=1 num_extra_inhabitants=0 bitwise_takable=1))))
// CHECK32-NEXT:               (field name=_flags offset=10
// CHECK32-NEXT:                 (struct size=2 alignment=2 stride=2 num_extra_inhabitants=0 bitwise_takable=1
// CHECK32-NEXT:                   (field name=_value offset=0
// CHECK32-NEXT:                     (builtin size=2 alignment=2 stride=2 num_extra_inhabitants=0 bitwise_takable=1)))))))))))

enum VastNumberOfCasesNoPayload {
  case option0
  case option1
  case option2
  case option3
  case option4
  case option5
  case option6
  case option7
  case option8
  case option9
  case option10
  case option11
  case option12
  case option13
  case option14
  case option15
  case option16
  case option17
  case option18
  case option19
  case option20
  case option21
  case option22
  case option23
  case option24
  case option25
  case option26
  case option27
  case option28
  case option29
  case option30
  case option31
  case option32
  case option33
  case option34
  case option35
  case option36
  case option37
  case option38
  case option39
  case option40
  case option41
  case option42
  case option43
  case option44
  case option45
  case option46
  case option47
  case option48
  case option49
  case option50
  case option51
  case option52
  case option53
  case option54
  case option55
  case option56
  case option57
  case option58
  case option59
  case option60
  case option61
  case option62
  case option63
  case option64
  case option65
  case option66
  case option67
  case option68
  case option69
  case option70
  case option71
  case option72
  case option73
  case option74
  case option75
  case option76
  case option77
  case option78
  case option79
  case option80
  case option81
  case option82
  case option83
  case option84
  case option85
  case option86
  case option87
  case option88
  case option89
  case option90
  case option91
  case option92
  case option93
  case option94
  case option95
  case option96
  case option97
  case option98
  case option99
  case option100
  case option101
  case option102
  case option103
  case option104
  case option105
  case option106
  case option107
  case option108
  case option109
  case option110
  case option111
  case option112
  case option113
  case option114
  case option115
  case option116
  case option117
  case option118
  case option119
  case option120
  case option121
  case option122
  case option123
  case option124
  case option125
  case option126
  case option127
  case option128
  case option129
  case option130
  case option131
  case option132
  case option133
  case option134
  case option135
  case option136
  case option137
  case option138
  case option139
  case option140
  case option141
  case option142
  case option143
  case option144
  case option145
  case option146
  case option147
  case option148
  case option149
  case option150
  case option151
  case option152
  case option153
  case option154
  case option155
  case option156
  case option157
  case option158
  case option159
  case option160
  case option161
  case option162
  case option163
  case option164
  case option165
  case option166
  case option167
  case option168
  case option169
  case option170
  case option171
  case option172
  case option173
  case option174
  case option175
  case option176
  case option177
  case option178
  case option179
  case option180
  case option181
  case option182
  case option183
  case option184
  case option185
  case option186
  case option187
  case option188
  case option189
  case option190
  case option191
  case option192
  case option193
  case option194
  case option195
  case option196
  case option197
  case option198
  case option199
  case option200
  case option201
  case option202
  case option203
  case option204
  case option205
  case option206
  case option207
  case option208
  case option209
  case option210
  case option211
  case option212
  case option213
  case option214
  case option215
  case option216
  case option217
  case option218
  case option219
  case option220
  case option221
  case option222
  case option223
  case option224
  case option225
  case option226
  case option227
  case option228
  case option229
  case option230
  case option231
  case option232
  case option233
  case option234
  case option235
  case option236
  case option237
  case option238
  case option239
  case option240
  case option241
  case option242
  case option243
  case option244
  case option245
  case option246
  case option247
  case option248
  case option249
  case option250
  case option251
  case option252
  case option253
  case option254
  case option255
  case option256
  case option257
}

enum ManyCasesOneIntPayload {
case payload(Int)
case otherA, otherB, otherC
}

enum ManyCasesOneStringPayload {
  case payload(String)
  case otherA, otherB, otherC
}
class ManyCasesOnePayloadC {
  var payload = ManyCasesOneStringPayload.payload("testString")
  var a = ManyCasesOneStringPayload.otherA
  var b = ManyCasesOneStringPayload.otherB
  var c = ManyCasesOneStringPayload.otherC
}
reflect(object: ManyCasesOnePayloadC())

// CHECKALL: Reflecting an object.
// CHECKALL-NEXT: Instance pointer in child address space: 0x{{[0-9a-fA-F]+}}
// CHECKALL-NEXT: Type reference:
// CHECKALL-NEXT: (class reflect_Enum_values.ManyCasesOnePayloadC)

// CHECKALL: Type info:
// CHECK64-NEXT: (class_instance size=80 alignment=8 stride=80 num_extra_inhabitants=0 bitwise_takable=1
// CHECK64-NEXT:   (field name=payload offset=16
// CHECK64-NEXT:     (single_payload_enum size=16 alignment=8 stride=16 num_extra_inhabitants=[[#num_extra_inhabitants_64bit-3]] bitwise_takable=1
// CHECK64-NEXT:       (case name=payload index=0 offset=0
// CHECK64-NEXT:         (struct size=16 alignment=8 stride=16 num_extra_inhabitants=[[#num_extra_inhabitants_64bit]] bitwise_takable=1
// CHECK64-NEXT:           (field name=_guts offset=0
// CHECK64-NEXT:             (struct size=16 alignment=8 stride=16 num_extra_inhabitants=[[#num_extra_inhabitants_64bit]] bitwise_takable=1
// CHECK64-NEXT:               (field name=_object offset=0
// CHECK64-NEXT:                 (struct size=16 alignment=8 stride=16 num_extra_inhabitants=[[#num_extra_inhabitants_64bit]] bitwise_takable=1
// CHECK64-NEXT:                   (field name=_countAndFlagsBits offset=0
// CHECK64-NEXT:                     (struct size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1
// CHECK64-NEXT:                       (field name=_value offset=0
// CHECK64-NEXT:                         (builtin size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1))))
// CHECK64-NEXT:                   (field name=_object offset=8
// CHECK64-NEXT:                     (builtin size=8 alignment=8 stride=8 num_extra_inhabitants=[[#num_extra_inhabitants_64bit]] bitwise_takable=1))))))))
// CHECK64-NEXT:       (case name=otherA index=1)
// CHECK64-NEXT:       (case name=otherB index=2)
// CHECK64-NEXT:       (case name=otherC index=3)))
// CHECK64-NEXT:   (field name=a offset=32
// CHECK64-NEXT:     (single_payload_enum size=16 alignment=8 stride=16 num_extra_inhabitants=[[#num_extra_inhabitants_64bit-3]] bitwise_takable=1
// CHECK64-NEXT:       (case name=payload index=0 offset=0
// CHECK64-NEXT:         (struct size=16 alignment=8 stride=16 num_extra_inhabitants=[[#num_extra_inhabitants_64bit]] bitwise_takable=1
// CHECK64-NEXT:           (field name=_guts offset=0
// CHECK64-NEXT:             (struct size=16 alignment=8 stride=16 num_extra_inhabitants=[[#num_extra_inhabitants_64bit]] bitwise_takable=1
// CHECK64-NEXT:               (field name=_object offset=0
// CHECK64-NEXT:                 (struct size=16 alignment=8 stride=16 num_extra_inhabitants=[[#num_extra_inhabitants_64bit]] bitwise_takable=1
// CHECK64-NEXT:                   (field name=_countAndFlagsBits offset=0
// CHECK64-NEXT:                     (struct size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1
// CHECK64-NEXT:                       (field name=_value offset=0
// CHECK64-NEXT:                         (builtin size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1))))
// CHECK64-NEXT:                   (field name=_object offset=8
// CHECK64-NEXT:                     (builtin size=8 alignment=8 stride=8 num_extra_inhabitants=[[#num_extra_inhabitants_64bit]] bitwise_takable=1))))))))
// CHECK64-NEXT:       (case name=otherA index=1)
// CHECK64-NEXT:       (case name=otherB index=2)
// CHECK64-NEXT:       (case name=otherC index=3)))
// CHECK64-NEXT:   (field name=b offset=48
// CHECK64-NEXT:     (single_payload_enum size=16 alignment=8 stride=16 num_extra_inhabitants=[[#num_extra_inhabitants_64bit-3]] bitwise_takable=1
// CHECK64-NEXT:       (case name=payload index=0 offset=0
// CHECK64-NEXT:         (struct size=16 alignment=8 stride=16 num_extra_inhabitants=[[#num_extra_inhabitants_64bit]] bitwise_takable=1
// CHECK64-NEXT:           (field name=_guts offset=0
// CHECK64-NEXT:             (struct size=16 alignment=8 stride=16 num_extra_inhabitants=[[#num_extra_inhabitants_64bit]] bitwise_takable=1
// CHECK64-NEXT:               (field name=_object offset=0
// CHECK64-NEXT:                 (struct size=16 alignment=8 stride=16 num_extra_inhabitants=[[#num_extra_inhabitants_64bit]] bitwise_takable=1
// CHECK64-NEXT:                   (field name=_countAndFlagsBits offset=0
// CHECK64-NEXT:                     (struct size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1
// CHECK64-NEXT:                       (field name=_value offset=0
// CHECK64-NEXT:                         (builtin size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1))))
// CHECK64-NEXT:                   (field name=_object offset=8
// CHECK64-NEXT:                     (builtin size=8 alignment=8 stride=8 num_extra_inhabitants=[[#num_extra_inhabitants_64bit]] bitwise_takable=1))))))))
// CHECK64-NEXT:       (case name=otherA index=1)
// CHECK64-NEXT:       (case name=otherB index=2)
// CHECK64-NEXT:       (case name=otherC index=3)))
// CHECK64-NEXT:   (field name=c offset=64
// CHECK64-NEXT:     (single_payload_enum size=16 alignment=8 stride=16 num_extra_inhabitants=[[#num_extra_inhabitants_64bit-3]] bitwise_takable=1
// CHECK64-NEXT:       (case name=payload index=0 offset=0
// CHECK64-NEXT:         (struct size=16 alignment=8 stride=16 num_extra_inhabitants=[[#num_extra_inhabitants_64bit]] bitwise_takable=1
// CHECK64-NEXT:           (field name=_guts offset=0
// CHECK64-NEXT:             (struct size=16 alignment=8 stride=16 num_extra_inhabitants=[[#num_extra_inhabitants_64bit]] bitwise_takable=1
// CHECK64-NEXT:               (field name=_object offset=0
// CHECK64-NEXT:                 (struct size=16 alignment=8 stride=16 num_extra_inhabitants=[[#num_extra_inhabitants_64bit]] bitwise_takable=1
// CHECK64-NEXT:                   (field name=_countAndFlagsBits offset=0
// CHECK64-NEXT:                     (struct size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1
// CHECK64-NEXT:                       (field name=_value offset=0
// CHECK64-NEXT:                         (builtin size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1))))
// CHECK64-NEXT:                   (field name=_object offset=8
// CHECK64-NEXT:                     (builtin size=8 alignment=8 stride=8 num_extra_inhabitants=[[#num_extra_inhabitants_64bit]] bitwise_takable=1)))))))
// CHECK64-NEXT:       (case name=otherA index=1)
// CHECK64-NEXT:       (case name=otherB index=2)
// CHECK64-NEXT:       (case name=otherC index=3)))

// CHECK32-NEXT: (class_instance size=56 alignment=4 stride=56 num_extra_inhabitants=0 bitwise_takable=1
// CHECK32-NEXT:   (field name=payload offset=8
// CHECK32-NEXT:     (single_payload_enum size=12 alignment=4 stride=12 num_extra_inhabitants=250 bitwise_takable=1
// CHECK32-NEXT:       (case name=payload index=0 offset=0
// CHECK32-NEXT:         (struct size=12 alignment=4 stride=12 num_extra_inhabitants=253 bitwise_takable=1
// CHECK32-NEXT:           (field name=_guts offset=0
// CHECK32-NEXT:             (struct size=12 alignment=4 stride=12 num_extra_inhabitants=253 bitwise_takable=1
// CHECK32-NEXT:               (field name=_object offset=0
// CHECK32-NEXT:                 (struct size=12 alignment=4 stride=12 num_extra_inhabitants=253 bitwise_takable=1
// CHECK32-NEXT:                   (field name=_count offset=0
// CHECK32-NEXT:                     (struct size=4 alignment=4 stride=4 num_extra_inhabitants=0 bitwise_takable=1
// CHECK32-NEXT:                       (field name=_value offset=0
// CHECK32-NEXT:                         (builtin size=4 alignment=4 stride=4 num_extra_inhabitants=0 bitwise_takable=1))))
// CHECK32-NEXT:                   (field name=_variant offset=4
// CHECK32-NEXT:                     (multi_payload_enum size=5 alignment=4 stride=8 num_extra_inhabitants=253 bitwise_takable=1
// CHECK32-NEXT:                       (case name=immortal index=0 offset=0
// CHECK32-NEXT:                         (struct size=4 alignment=4 stride=4 num_extra_inhabitants=0 bitwise_takable=1
// CHECK32-NEXT:                           (field name=_value offset=0
// CHECK32-NEXT:                             (builtin size=4 alignment=4 stride=4 num_extra_inhabitants=0 bitwise_takable=1))))
// CHECK32-NEXT:                       (case name=native index=1 offset=0
// CHECK32-NEXT:                         (class_existential size=4 alignment=4 stride=4 num_extra_inhabitants=4096 bitwise_takable=1
// CHECK32-NEXT:                           (field name=object offset=0
// CHECK32-NEXT:                             (reference kind=strong refcounting=unknown))))
// CHECK32-NEXT:                       (case name=bridged index=2 offset=0
// CHECK32-NEXT:                         (class_existential size=4 alignment=4 stride=4 num_extra_inhabitants=4096 bitwise_takable=1
// CHECK32-NEXT:                           (field name=object offset=0
// CHECK32-NEXT:                             (reference kind=strong refcounting=unknown))))))
// CHECK32-NEXT:                   (field name=_discriminator offset=9
// CHECK32-NEXT:                     (struct size=1 alignment=1 stride=1 num_extra_inhabitants=0 bitwise_takable=1
// CHECK32-NEXT:                       (field name=_value offset=0
// CHECK32-NEXT:                         (builtin size=1 alignment=1 stride=1 num_extra_inhabitants=0 bitwise_takable=1))))
// CHECK32-NEXT:                   (field name=_flags offset=10
// CHECK32-NEXT:                     (struct size=2 alignment=2 stride=2 num_extra_inhabitants=0 bitwise_takable=1
// CHECK32-NEXT:                       (field name=_value offset=0
// CHECK32-NEXT:                         (builtin size=2 alignment=2 stride=2 num_extra_inhabitants=0 bitwise_takable=1))))))))))
// CHECK32-NEXT:       (case name=otherA index=1)
// CHECK32-NEXT:       (case name=otherB index=2)
// CHECK32-NEXT:       (case name=otherC index=3)))
// CHECK32-NEXT:   (field name=a offset=20
// CHECK32-NEXT:     (single_payload_enum size=12 alignment=4 stride=12 num_extra_inhabitants=250 bitwise_takable=1
// CHECK32-NEXT:       (case name=payload index=0 offset=0
// CHECK32-NEXT:         (struct size=12 alignment=4 stride=12 num_extra_inhabitants=253 bitwise_takable=1
// CHECK32-NEXT:           (field name=_guts offset=0
// CHECK32-NEXT:             (struct size=12 alignment=4 stride=12 num_extra_inhabitants=253 bitwise_takable=1
// CHECK32-NEXT:               (field name=_object offset=0
// CHECK32-NEXT:                 (struct size=12 alignment=4 stride=12 num_extra_inhabitants=253 bitwise_takable=1
// CHECK32-NEXT:                   (field name=_count offset=0
// CHECK32-NEXT:                     (struct size=4 alignment=4 stride=4 num_extra_inhabitants=0 bitwise_takable=1
// CHECK32-NEXT:                       (field name=_value offset=0
// CHECK32-NEXT:                         (builtin size=4 alignment=4 stride=4 num_extra_inhabitants=0 bitwise_takable=1))))
// CHECK32-NEXT:                   (field name=_variant offset=4
// CHECK32-NEXT:                     (multi_payload_enum size=5 alignment=4 stride=8 num_extra_inhabitants=253 bitwise_takable=1
// CHECK32-NEXT:                       (case name=immortal index=0 offset=0
// CHECK32-NEXT:                         (struct size=4 alignment=4 stride=4 num_extra_inhabitants=0 bitwise_takable=1
// CHECK32-NEXT:                           (field name=_value offset=0
// CHECK32-NEXT:                             (builtin size=4 alignment=4 stride=4 num_extra_inhabitants=0 bitwise_takable=1))))
// CHECK32-NEXT:                       (case name=native index=1 offset=0
// CHECK32-NEXT:                         (class_existential size=4 alignment=4 stride=4 num_extra_inhabitants=4096 bitwise_takable=1
// CHECK32-NEXT:                           (field name=object offset=0
// CHECK32-NEXT:                             (reference kind=strong refcounting=unknown))))
// CHECK32-NEXT:                       (case name=bridged index=2 offset=0
// CHECK32-NEXT:                         (class_existential size=4 alignment=4 stride=4 num_extra_inhabitants=4096 bitwise_takable=1
// CHECK32-NEXT:                           (field name=object offset=0
// CHECK32-NEXT:                             (reference kind=strong refcounting=unknown))))))
// CHECK32-NEXT:                   (field name=_discriminator offset=9
// CHECK32-NEXT:                     (struct size=1 alignment=1 stride=1 num_extra_inhabitants=0 bitwise_takable=1
// CHECK32-NEXT:                       (field name=_value offset=0
// CHECK32-NEXT:                         (builtin size=1 alignment=1 stride=1 num_extra_inhabitants=0 bitwise_takable=1))))
// CHECK32-NEXT:                   (field name=_flags offset=10
// CHECK32-NEXT:                     (struct size=2 alignment=2 stride=2 num_extra_inhabitants=0 bitwise_takable=1
// CHECK32-NEXT:                       (field name=_value offset=0
// CHECK32-NEXT:                         (builtin size=2 alignment=2 stride=2 num_extra_inhabitants=0 bitwise_takable=1))))))))))
// CHECK32-NEXT:       (case name=otherA index=1)
// CHECK32-NEXT:       (case name=otherB index=2)
// CHECK32-NEXT:       (case name=otherC index=3)))
// CHECK32-NEXT:   (field name=b offset=32
// CHECK32-NEXT:     (single_payload_enum size=12 alignment=4 stride=12 num_extra_inhabitants=250 bitwise_takable=1
// CHECK32-NEXT:       (case name=payload index=0 offset=0
// CHECK32-NEXT:         (struct size=12 alignment=4 stride=12 num_extra_inhabitants=253 bitwise_takable=1
// CHECK32-NEXT:           (field name=_guts offset=0
// CHECK32-NEXT:             (struct size=12 alignment=4 stride=12 num_extra_inhabitants=253 bitwise_takable=1
// CHECK32-NEXT:               (field name=_object offset=0
// CHECK32-NEXT:                 (struct size=12 alignment=4 stride=12 num_extra_inhabitants=253 bitwise_takable=1
// CHECK32-NEXT:                   (field name=_count offset=0
// CHECK32-NEXT:                     (struct size=4 alignment=4 stride=4 num_extra_inhabitants=0 bitwise_takable=1
// CHECK32-NEXT:                       (field name=_value offset=0
// CHECK32-NEXT:                         (builtin size=4 alignment=4 stride=4 num_extra_inhabitants=0 bitwise_takable=1))))
// CHECK32-NEXT:                   (field name=_variant offset=4
// CHECK32-NEXT:                     (multi_payload_enum size=5 alignment=4 stride=8 num_extra_inhabitants=253 bitwise_takable=1
// CHECK32-NEXT:                       (case name=immortal index=0 offset=0
// CHECK32-NEXT:                         (struct size=4 alignment=4 stride=4 num_extra_inhabitants=0 bitwise_takable=1
// CHECK32-NEXT:                           (field name=_value offset=0
// CHECK32-NEXT:                             (builtin size=4 alignment=4 stride=4 num_extra_inhabitants=0 bitwise_takable=1))))
// CHECK32-NEXT:                       (case name=native index=1 offset=0
// CHECK32-NEXT:                         (class_existential size=4 alignment=4 stride=4 num_extra_inhabitants=4096 bitwise_takable=1
// CHECK32-NEXT:                           (field name=object offset=0
// CHECK32-NEXT:                             (reference kind=strong refcounting=unknown))))
// CHECK32-NEXT:                       (case name=bridged index=2 offset=0
// CHECK32-NEXT:                         (class_existential size=4 alignment=4 stride=4 num_extra_inhabitants=4096 bitwise_takable=1
// CHECK32-NEXT:                           (field name=object offset=0
// CHECK32-NEXT:                             (reference kind=strong refcounting=unknown))))))
// CHECK32-NEXT:                   (field name=_discriminator offset=9
// CHECK32-NEXT:                     (struct size=1 alignment=1 stride=1 num_extra_inhabitants=0 bitwise_takable=1
// CHECK32-NEXT:                       (field name=_value offset=0
// CHECK32-NEXT:                         (builtin size=1 alignment=1 stride=1 num_extra_inhabitants=0 bitwise_takable=1))))
// CHECK32-NEXT:                   (field name=_flags offset=10
// CHECK32-NEXT:                     (struct size=2 alignment=2 stride=2 num_extra_inhabitants=0 bitwise_takable=1
// CHECK32-NEXT:                       (field name=_value offset=0
// CHECK32-NEXT:                         (builtin size=2 alignment=2 stride=2 num_extra_inhabitants=0 bitwise_takable=1))))))))))
// CHECK32-NEXT:       (case name=otherA index=1)
// CHECK32-NEXT:       (case name=otherB index=2)
// CHECK32-NEXT:       (case name=otherC index=3)))
// CHECK32-NEXT:   (field name=c offset=44
// CHECK32-NEXT:     (single_payload_enum size=12 alignment=4 stride=12 num_extra_inhabitants=250 bitwise_takable=1
// CHECK32-NEXT:       (case name=payload index=0 offset=0
// CHECK32-NEXT:         (struct size=12 alignment=4 stride=12 num_extra_inhabitants=253 bitwise_takable=1
// CHECK32-NEXT:           (field name=_guts offset=0
// CHECK32-NEXT:             (struct size=12 alignment=4 stride=12 num_extra_inhabitants=253 bitwise_takable=1
// CHECK32-NEXT:               (field name=_object offset=0
// CHECK32-NEXT:                 (struct size=12 alignment=4 stride=12 num_extra_inhabitants=253 bitwise_takable=1
// CHECK32-NEXT:                   (field name=_count offset=0
// CHECK32-NEXT:                     (struct size=4 alignment=4 stride=4 num_extra_inhabitants=0 bitwise_takable=1
// CHECK32-NEXT:                       (field name=_value offset=0
// CHECK32-NEXT:                         (builtin size=4 alignment=4 stride=4 num_extra_inhabitants=0 bitwise_takable=1))))
// CHECK32-NEXT:                   (field name=_variant offset=4
// CHECK32-NEXT:                     (multi_payload_enum size=5 alignment=4 stride=8 num_extra_inhabitants=253 bitwise_takable=1
// CHECK32-NEXT:                       (case name=immortal index=0 offset=0
// CHECK32-NEXT:                         (struct size=4 alignment=4 stride=4 num_extra_inhabitants=0 bitwise_takable=1
// CHECK32-NEXT:                           (field name=_value offset=0
// CHECK32-NEXT:                             (builtin size=4 alignment=4 stride=4 num_extra_inhabitants=0 bitwise_takable=1))))
// CHECK32-NEXT:                       (case name=native index=1 offset=0
// CHECK32-NEXT:                         (class_existential size=4 alignment=4 stride=4 num_extra_inhabitants=4096 bitwise_takable=1
// CHECK32-NEXT:                           (field name=object offset=0
// CHECK32-NEXT:                             (reference kind=strong refcounting=unknown))))
// CHECK32-NEXT:                       (case name=bridged index=2 offset=0
// CHECK32-NEXT:                         (class_existential size=4 alignment=4 stride=4 num_extra_inhabitants=4096 bitwise_takable=1
// CHECK32-NEXT:                           (field name=object offset=0
// CHECK32-NEXT:                             (reference kind=strong refcounting=unknown))))))
// CHECK32-NEXT:                   (field name=_discriminator offset=9
// CHECK32-NEXT:                     (struct size=1 alignment=1 stride=1 num_extra_inhabitants=0 bitwise_takable=1
// CHECK32-NEXT:                       (field name=_value offset=0
// CHECK32-NEXT:                         (builtin size=1 alignment=1 stride=1 num_extra_inhabitants=0 bitwise_takable=1))))
// CHECK32-NEXT:                   (field name=_flags offset=10
// CHECK32-NEXT:                     (struct size=2 alignment=2 stride=2 num_extra_inhabitants=0 bitwise_takable=1
// CHECK32-NEXT:                       (field name=_value offset=0
// CHECK32-NEXT:                         (builtin size=2 alignment=2 stride=2 num_extra_inhabitants=0 bitwise_takable=1))))))))))
// CHECK32-NEXT:       (case name=otherA index=1)
// CHECK32-NEXT:       (case name=otherB index=2)
// CHECK32-NEXT:       (case name=otherC index=3))))

enum ManyCasesManyPayloads {
  case a(String)
  case b([Int])
  case extra
  case c([String: String])
}
class ManyCasesManyPayloadsC {
  var a = ManyCasesManyPayloads.a("testString")
  var b = ManyCasesManyPayloads.b([10, 20, 30])
  var c = ManyCasesManyPayloads.c(["name": "Telephone", "purpose": "Bothering"])
}
reflect(object: ManyCasesManyPayloadsC())

// CHECKALL: Reflecting an object.
// CHECKALL-NEXT: Instance pointer in child address space: 0x{{[0-9a-fA-F]+}}
// CHECKALL-NEXT: Type reference:
// CHECKALL-NEXT: (class reflect_Enum_values.ManyCasesManyPayloadsC)

// CHECKALL: Type info:
// CHECK64-NEXT: (class_instance size=81 alignment=8 stride=88 num_extra_inhabitants=0 bitwise_takable=1
// CHECK64-NEXT:   (field name=a offset=16
// CHECK64-NEXT:     (multi_payload_enum size=17 alignment=8 stride=24 num_extra_inhabitants=252 bitwise_takable=1
// CHECK64-NEXT:       (case name=a index=0 offset=0
// CHECK64-NEXT:         (struct size=16 alignment=8 stride=16 num_extra_inhabitants=[[#num_extra_inhabitants_64bit]] bitwise_takable=1
// CHECK64-NEXT:           (field name=_guts offset=0
// CHECK64-NEXT:             (struct size=16 alignment=8 stride=16 num_extra_inhabitants=[[#num_extra_inhabitants_64bit]] bitwise_takable=1
// CHECK64-NEXT:               (field name=_object offset=0
// CHECK64-NEXT:                 (struct size=16 alignment=8 stride=16 num_extra_inhabitants=[[#num_extra_inhabitants_64bit]] bitwise_takable=1
// CHECK64-NEXT:                   (field name=_countAndFlagsBits offset=0
// CHECK64-NEXT:                     (struct size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1
// CHECK64-NEXT:                       (field name=_value offset=0
// CHECK64-NEXT:                         (builtin size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1))))
// CHECK64-NEXT:                   (field name=_object offset=8
// CHECK64-NEXT:                     (builtin size=8 alignment=8 stride=8 num_extra_inhabitants=[[#num_extra_inhabitants_64bit]] bitwise_takable=1))))))))
// CHECK64-NEXT:       (case name=b index=1 offset=0
// CHECK64-NEXT:         (struct size=8 alignment=8 stride=8 num_extra_inhabitants=[[#num_extra_inhabitants_64bit]] bitwise_takable=1
// CHECK64-NEXT:           (field name=_buffer offset=0
// CHECK64-NEXT:             (struct size=8 alignment=8 stride=8 num_extra_inhabitants=[[#num_extra_inhabitants_64bit]] bitwise_takable=1
// CHECK64-NEXT:               (field name=_storage offset=0
// CHECK64-NEXT:                 (struct size=8 alignment=8 stride=8 num_extra_inhabitants=[[#num_extra_inhabitants_64bit]] bitwise_takable=1
// CHECK64-NEXT:                   (field name=rawValue offset=0
// CHECK64-NEXT:                     (builtin size=8 alignment=8 stride=8 num_extra_inhabitants=[[#num_extra_inhabitants_64bit]] bitwise_takable=1))))))))
// CHECK64-NEXT:       (case name=c index=2 offset=0
// CHECK64-NEXT:         (struct size=8 alignment=8 stride=8 num_extra_inhabitants=[[#num_extra_inhabitants_64bit]] bitwise_takable=1
// CHECK64-NEXT:           (field name=_variant offset=0
// CHECK64-NEXT:             (struct size=8 alignment=8 stride=8 num_extra_inhabitants=[[#num_extra_inhabitants_64bit]] bitwise_takable=1
// CHECK64-NEXT:               (field name=object offset=0
// CHECK64-NEXT:                 (struct size=8 alignment=8 stride=8 num_extra_inhabitants=[[#num_extra_inhabitants_64bit]] bitwise_takable=1
// CHECK64-NEXT:                   (field name=rawValue offset=0
// CHECK64-NEXT:                     (builtin size=8 alignment=8 stride=8 num_extra_inhabitants=[[#num_extra_inhabitants_64bit]] bitwise_takable=1))))))))
// CHECK64-NEXT:       (case name=extra index=3)))
// CHECK64-NEXT:   (field name=b offset=40
// CHECK64-NEXT:     (multi_payload_enum size=17 alignment=8 stride=24 num_extra_inhabitants=252 bitwise_takable=1
// CHECK64-NEXT:       (case name=a index=0 offset=0
// CHECK64-NEXT:         (struct size=16 alignment=8 stride=16 num_extra_inhabitants=[[#num_extra_inhabitants_64bit]] bitwise_takable=1
// CHECK64-NEXT:           (field name=_guts offset=0
// CHECK64-NEXT:             (struct size=16 alignment=8 stride=16 num_extra_inhabitants=[[#num_extra_inhabitants_64bit]] bitwise_takable=1
// CHECK64-NEXT:               (field name=_object offset=0
// CHECK64-NEXT:                 (struct size=16 alignment=8 stride=16 num_extra_inhabitants=[[#num_extra_inhabitants_64bit]] bitwise_takable=1
// CHECK64-NEXT:                   (field name=_countAndFlagsBits offset=0
// CHECK64-NEXT:                     (struct size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1
// CHECK64-NEXT:                       (field name=_value offset=0
// CHECK64-NEXT:                         (builtin size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1))))
// CHECK64-NEXT:                   (field name=_object offset=8
// CHECK64-NEXT:                     (builtin size=8 alignment=8 stride=8 num_extra_inhabitants=[[#num_extra_inhabitants_64bit]] bitwise_takable=1))))))))
// CHECK64-NEXT:       (case name=b index=1 offset=0
// CHECK64-NEXT:         (struct size=8 alignment=8 stride=8 num_extra_inhabitants=[[#num_extra_inhabitants_64bit]] bitwise_takable=1
// CHECK64-NEXT:           (field name=_buffer offset=0
// CHECK64-NEXT:             (struct size=8 alignment=8 stride=8 num_extra_inhabitants=[[#num_extra_inhabitants_64bit]] bitwise_takable=1
// CHECK64-NEXT:               (field name=_storage offset=0
// CHECK64-NEXT:                 (struct size=8 alignment=8 stride=8 num_extra_inhabitants=[[#num_extra_inhabitants_64bit]] bitwise_takable=1
// CHECK64-NEXT:                   (field name=rawValue offset=0
// CHECK64-NEXT:                     (builtin size=8 alignment=8 stride=8 num_extra_inhabitants=[[#num_extra_inhabitants_64bit]] bitwise_takable=1))))))))
// CHECK64-NEXT:       (case name=c index=2 offset=0
// CHECK64-NEXT:         (struct size=8 alignment=8 stride=8 num_extra_inhabitants=[[#num_extra_inhabitants_64bit]] bitwise_takable=1
// CHECK64-NEXT:           (field name=_variant offset=0
// CHECK64-NEXT:             (struct size=8 alignment=8 stride=8 num_extra_inhabitants=[[#num_extra_inhabitants_64bit]] bitwise_takable=1
// CHECK64-NEXT:               (field name=object offset=0
// CHECK64-NEXT:                 (struct size=8 alignment=8 stride=8 num_extra_inhabitants=[[#num_extra_inhabitants_64bit]] bitwise_takable=1
// CHECK64-NEXT:                   (field name=rawValue offset=0
// CHECK64-NEXT:                     (builtin size=8 alignment=8 stride=8 num_extra_inhabitants=[[#num_extra_inhabitants_64bit]] bitwise_takable=1))))))))
// CHECK64-NEXT:       (case name=extra index=3)))
// CHECK64-NEXT:   (field name=c offset=64
// CHECK64-NEXT:     (multi_payload_enum size=17 alignment=8 stride=24 num_extra_inhabitants=252 bitwise_takable=1
// CHECK64-NEXT:       (case name=a index=0 offset=0
// CHECK64-NEXT:         (struct size=16 alignment=8 stride=16 num_extra_inhabitants=[[#num_extra_inhabitants_64bit]] bitwise_takable=1
// CHECK64-NEXT:           (field name=_guts offset=0
// CHECK64-NEXT:             (struct size=16 alignment=8 stride=16 num_extra_inhabitants=[[#num_extra_inhabitants_64bit]] bitwise_takable=1
// CHECK64-NEXT:               (field name=_object offset=0
// CHECK64-NEXT:                 (struct size=16 alignment=8 stride=16 num_extra_inhabitants=[[#num_extra_inhabitants_64bit]] bitwise_takable=1
// CHECK64-NEXT:                   (field name=_countAndFlagsBits offset=0
// CHECK64-NEXT:                     (struct size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1
// CHECK64-NEXT:                       (field name=_value offset=0
// CHECK64-NEXT:                         (builtin size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1))))
// CHECK64-NEXT:                   (field name=_object offset=8
// CHECK64-NEXT:                     (builtin size=8 alignment=8 stride=8 num_extra_inhabitants=[[#num_extra_inhabitants_64bit]] bitwise_takable=1))))))))
// CHECK64-NEXT:       (case name=b index=1 offset=0
// CHECK64-NEXT:         (struct size=8 alignment=8 stride=8 num_extra_inhabitants=[[#num_extra_inhabitants_64bit]] bitwise_takable=1
// CHECK64-NEXT:           (field name=_buffer offset=0
// CHECK64-NEXT:             (struct size=8 alignment=8 stride=8 num_extra_inhabitants=[[#num_extra_inhabitants_64bit]] bitwise_takable=1
// CHECK64-NEXT:               (field name=_storage offset=0
// CHECK64-NEXT:                 (struct size=8 alignment=8 stride=8 num_extra_inhabitants=[[#num_extra_inhabitants_64bit]] bitwise_takable=1
// CHECK64-NEXT:                   (field name=rawValue offset=0
// CHECK64-NEXT:                     (builtin size=8 alignment=8 stride=8 num_extra_inhabitants=[[#num_extra_inhabitants_64bit]] bitwise_takable=1))))))))
// CHECK64-NEXT:       (case name=c index=2 offset=0
// CHECK64-NEXT:         (struct size=8 alignment=8 stride=8 num_extra_inhabitants=[[#num_extra_inhabitants_64bit]] bitwise_takable=1
// CHECK64-NEXT:           (field name=_variant offset=0
// CHECK64-NEXT:             (struct size=8 alignment=8 stride=8 num_extra_inhabitants=[[#num_extra_inhabitants_64bit]] bitwise_takable=1
// CHECK64-NEXT:               (field name=object offset=0
// CHECK64-NEXT:                 (struct size=8 alignment=8 stride=8 num_extra_inhabitants=[[#num_extra_inhabitants_64bit]] bitwise_takable=1
// CHECK64-NEXT:                   (field name=rawValue offset=0
// CHECK64-NEXT:                     (builtin size=8 alignment=8 stride=8 num_extra_inhabitants=[[#num_extra_inhabitants_64bit]] bitwise_takable=1))))))))
// CHECK64-NEXT:       (case name=extra index=3))))

// CHECK32-NEXT: (class_instance size=44 alignment=4 stride=44 num_extra_inhabitants=0 bitwise_takable=1
// CHECK32-NEXT:   (field name=a offset=8
// CHECK32-NEXT:     (multi_payload_enum size=12 alignment=4 stride=12 num_extra_inhabitants=60 bitwise_takable=1
// CHECK32-NEXT:       (case name=a index=0 offset=0
// CHECK32-NEXT:         (struct size=12 alignment=4 stride=12 num_extra_inhabitants=253 bitwise_takable=1
// CHECK32-NEXT:           (field name=_guts offset=0
// CHECK32-NEXT:             (struct size=12 alignment=4 stride=12 num_extra_inhabitants=253 bitwise_takable=1
// CHECK32-NEXT:               (field name=_object offset=0
// CHECK32-NEXT:                 (struct size=12 alignment=4 stride=12 num_extra_inhabitants=253 bitwise_takable=1
// CHECK32-NEXT:                   (field name=_count offset=0
// CHECK32-NEXT:                     (struct size=4 alignment=4 stride=4 num_extra_inhabitants=0 bitwise_takable=1
// CHECK32-NEXT:                       (field name=_value offset=0
// CHECK32-NEXT:                         (builtin size=4 alignment=4 stride=4 num_extra_inhabitants=0 bitwise_takable=1))))
// CHECK32-NEXT:                   (field name=_variant offset=4
// CHECK32-NEXT:                     (multi_payload_enum size=5 alignment=4 stride=8 num_extra_inhabitants=253 bitwise_takable=1
// CHECK32-NEXT:                       (case name=immortal index=0 offset=0
// CHECK32-NEXT:                         (struct size=4 alignment=4 stride=4 num_extra_inhabitants=0 bitwise_takable=1
// CHECK32-NEXT:                           (field name=_value offset=0
// CHECK32-NEXT:                             (builtin size=4 alignment=4 stride=4 num_extra_inhabitants=0 bitwise_takable=1))))
// CHECK32-NEXT:                       (case name=native index=1 offset=0
// CHECK32-NEXT:                         (class_existential size=4 alignment=4 stride=4 num_extra_inhabitants=4096 bitwise_takable=1
// CHECK32-NEXT:                           (field name=object offset=0
// CHECK32-NEXT:                             (reference kind=strong refcounting=unknown))))
// CHECK32-NEXT:                       (case name=bridged index=2 offset=0
// CHECK32-NEXT:                         (class_existential size=4 alignment=4 stride=4 num_extra_inhabitants=4096 bitwise_takable=1
// CHECK32-NEXT:                           (field name=object offset=0
// CHECK32-NEXT:                             (reference kind=strong refcounting=unknown))))))
// CHECK32-NEXT:                   (field name=_discriminator offset=9
// CHECK32-NEXT:                     (struct size=1 alignment=1 stride=1 num_extra_inhabitants=0 bitwise_takable=1
// CHECK32-NEXT:                       (field name=_value offset=0
// CHECK32-NEXT:                         (builtin size=1 alignment=1 stride=1 num_extra_inhabitants=0 bitwise_takable=1))))
// CHECK32-NEXT:                   (field name=_flags offset=10
// CHECK32-NEXT:                     (struct size=2 alignment=2 stride=2 num_extra_inhabitants=0 bitwise_takable=1
// CHECK32-NEXT:                       (field name=_value offset=0
// CHECK32-NEXT:                         (builtin size=2 alignment=2 stride=2 num_extra_inhabitants=0 bitwise_takable=1))))))))))
// CHECK32-NEXT:       (case name=b index=1 offset=0
// CHECK32-NEXT:         (struct size=4 alignment=4 stride=4 num_extra_inhabitants=4096 bitwise_takable=1
// CHECK32-NEXT:           (field name=_buffer offset=0
// CHECK32-NEXT:             (struct size=4 alignment=4 stride=4 num_extra_inhabitants=4096 bitwise_takable=1
// CHECK32-NEXT:               (field name=_storage offset=0
// CHECK32-NEXT:                 (struct size=4 alignment=4 stride=4 num_extra_inhabitants=4096 bitwise_takable=1
// CHECK32-NEXT:                   (field name=rawValue offset=0
// CHECK32-NEXT:                     (builtin size=4 alignment=4 stride=4 num_extra_inhabitants=4096 bitwise_takable=1))))))))
// CHECK32-NEXT:       (case name=c index=2 offset=0
// CHECK32-NEXT:         (struct size=4 alignment=4 stride=4 num_extra_inhabitants=4096 bitwise_takable=1
// CHECK32-NEXT:           (field name=_variant offset=0
// CHECK32-NEXT:             (struct size=4 alignment=4 stride=4 num_extra_inhabitants=4096 bitwise_takable=1
// CHECK32-NEXT:               (field name=object offset=0
// CHECK32-NEXT:                 (struct size=4 alignment=4 stride=4 num_extra_inhabitants=4096 bitwise_takable=1
// CHECK32-NEXT:                   (field name=rawValue offset=0
// CHECK32-NEXT:                     (builtin size=4 alignment=4 stride=4 num_extra_inhabitants=4096 bitwise_takable=1))))))))
// CHECK32-NEXT:       (case name=extra index=3)))
// CHECK32-NEXT:   (field name=b offset=20
// CHECK32-NEXT:     (multi_payload_enum size=12 alignment=4 stride=12 num_extra_inhabitants=60 bitwise_takable=1
// CHECK32-NEXT:       (case name=a index=0 offset=0
// CHECK32-NEXT:         (struct size=12 alignment=4 stride=12 num_extra_inhabitants=253 bitwise_takable=1
// CHECK32-NEXT:           (field name=_guts offset=0
// CHECK32-NEXT:             (struct size=12 alignment=4 stride=12 num_extra_inhabitants=253 bitwise_takable=1
// CHECK32-NEXT:               (field name=_object offset=0
// CHECK32-NEXT:                 (struct size=12 alignment=4 stride=12 num_extra_inhabitants=253 bitwise_takable=1
// CHECK32-NEXT:                   (field name=_count offset=0
// CHECK32-NEXT:                     (struct size=4 alignment=4 stride=4 num_extra_inhabitants=0 bitwise_takable=1
// CHECK32-NEXT:                       (field name=_value offset=0
// CHECK32-NEXT:                         (builtin size=4 alignment=4 stride=4 num_extra_inhabitants=0 bitwise_takable=1))))
// CHECK32-NEXT:                   (field name=_variant offset=4
// CHECK32-NEXT:                     (multi_payload_enum size=5 alignment=4 stride=8 num_extra_inhabitants=253 bitwise_takable=1
// CHECK32-NEXT:                       (case name=immortal index=0 offset=0
// CHECK32-NEXT:                         (struct size=4 alignment=4 stride=4 num_extra_inhabitants=0 bitwise_takable=1
// CHECK32-NEXT:                           (field name=_value offset=0
// CHECK32-NEXT:                             (builtin size=4 alignment=4 stride=4 num_extra_inhabitants=0 bitwise_takable=1))))
// CHECK32-NEXT:                       (case name=native index=1 offset=0
// CHECK32-NEXT:                         (class_existential size=4 alignment=4 stride=4 num_extra_inhabitants=4096 bitwise_takable=1
// CHECK32-NEXT:                           (field name=object offset=0
// CHECK32-NEXT:                             (reference kind=strong refcounting=unknown))))
// CHECK32-NEXT:                       (case name=bridged index=2 offset=0
// CHECK32-NEXT:                         (class_existential size=4 alignment=4 stride=4 num_extra_inhabitants=4096 bitwise_takable=1
// CHECK32-NEXT:                           (field name=object offset=0
// CHECK32-NEXT:                             (reference kind=strong refcounting=unknown))))))
// CHECK32-NEXT:                   (field name=_discriminator offset=9
// CHECK32-NEXT:                     (struct size=1 alignment=1 stride=1 num_extra_inhabitants=0 bitwise_takable=1
// CHECK32-NEXT:                       (field name=_value offset=0
// CHECK32-NEXT:                         (builtin size=1 alignment=1 stride=1 num_extra_inhabitants=0 bitwise_takable=1))))
// CHECK32-NEXT:                   (field name=_flags offset=10
// CHECK32-NEXT:                     (struct size=2 alignment=2 stride=2 num_extra_inhabitants=0 bitwise_takable=1
// CHECK32-NEXT:                       (field name=_value offset=0
// CHECK32-NEXT:                         (builtin size=2 alignment=2 stride=2 num_extra_inhabitants=0 bitwise_takable=1))))))))))
// CHECK32-NEXT:       (case name=b index=1 offset=0
// CHECK32-NEXT:         (struct size=4 alignment=4 stride=4 num_extra_inhabitants=4096 bitwise_takable=1
// CHECK32-NEXT:           (field name=_buffer offset=0
// CHECK32-NEXT:             (struct size=4 alignment=4 stride=4 num_extra_inhabitants=4096 bitwise_takable=1
// CHECK32-NEXT:               (field name=_storage offset=0
// CHECK32-NEXT:                 (struct size=4 alignment=4 stride=4 num_extra_inhabitants=4096 bitwise_takable=1
// CHECK32-NEXT:                   (field name=rawValue offset=0
// CHECK32-NEXT:                     (builtin size=4 alignment=4 stride=4 num_extra_inhabitants=4096 bitwise_takable=1))))))))
// CHECK32-NEXT:       (case name=c index=2 offset=0
// CHECK32-NEXT:         (struct size=4 alignment=4 stride=4 num_extra_inhabitants=4096 bitwise_takable=1
// CHECK32-NEXT:           (field name=_variant offset=0
// CHECK32-NEXT:             (struct size=4 alignment=4 stride=4 num_extra_inhabitants=4096 bitwise_takable=1
// CHECK32-NEXT:               (field name=object offset=0
// CHECK32-NEXT:                 (struct size=4 alignment=4 stride=4 num_extra_inhabitants=4096 bitwise_takable=1
// CHECK32-NEXT:                   (field name=rawValue offset=0
// CHECK32-NEXT:                     (builtin size=4 alignment=4 stride=4 num_extra_inhabitants=4096 bitwise_takable=1))))))))
// CHECK32-NEXT:       (case name=extra index=3)))
// CHECK32-NEXT:   (field name=c offset=32
// CHECK32-NEXT:     (multi_payload_enum size=12 alignment=4 stride=12 num_extra_inhabitants=60 bitwise_takable=1
// CHECK32-NEXT:       (case name=a index=0 offset=0
// CHECK32-NEXT:         (struct size=12 alignment=4 stride=12 num_extra_inhabitants=253 bitwise_takable=1
// CHECK32-NEXT:           (field name=_guts offset=0
// CHECK32-NEXT:             (struct size=12 alignment=4 stride=12 num_extra_inhabitants=253 bitwise_takable=1
// CHECK32-NEXT:               (field name=_object offset=0
// CHECK32-NEXT:                 (struct size=12 alignment=4 stride=12 num_extra_inhabitants=253 bitwise_takable=1
// CHECK32-NEXT:                   (field name=_count offset=0
// CHECK32-NEXT:                     (struct size=4 alignment=4 stride=4 num_extra_inhabitants=0 bitwise_takable=1
// CHECK32-NEXT:                       (field name=_value offset=0
// CHECK32-NEXT:                         (builtin size=4 alignment=4 stride=4 num_extra_inhabitants=0 bitwise_takable=1))))
// CHECK32-NEXT:                   (field name=_variant offset=4
// CHECK32-NEXT:                     (multi_payload_enum size=5 alignment=4 stride=8 num_extra_inhabitants=253 bitwise_takable=1
// CHECK32-NEXT:                       (case name=immortal index=0 offset=0
// CHECK32-NEXT:                         (struct size=4 alignment=4 stride=4 num_extra_inhabitants=0 bitwise_takable=1
// CHECK32-NEXT:                           (field name=_value offset=0
// CHECK32-NEXT:                             (builtin size=4 alignment=4 stride=4 num_extra_inhabitants=0 bitwise_takable=1))))
// CHECK32-NEXT:                       (case name=native index=1 offset=0
// CHECK32-NEXT:                         (class_existential size=4 alignment=4 stride=4 num_extra_inhabitants=4096 bitwise_takable=1
// CHECK32-NEXT:                           (field name=object offset=0
// CHECK32-NEXT:                             (reference kind=strong refcounting=unknown))))
// CHECK32-NEXT:                       (case name=bridged index=2 offset=0
// CHECK32-NEXT:                         (class_existential size=4 alignment=4 stride=4 num_extra_inhabitants=4096 bitwise_takable=1
// CHECK32-NEXT:                           (field name=object offset=0
// CHECK32-NEXT:                             (reference kind=strong refcounting=unknown))))))
// CHECK32-NEXT:                   (field name=_discriminator offset=9
// CHECK32-NEXT:                     (struct size=1 alignment=1 stride=1 num_extra_inhabitants=0 bitwise_takable=1
// CHECK32-NEXT:                       (field name=_value offset=0
// CHECK32-NEXT:                         (builtin size=1 alignment=1 stride=1 num_extra_inhabitants=0 bitwise_takable=1))))
// CHECK32-NEXT:                   (field name=_flags offset=10
// CHECK32-NEXT:                     (struct size=2 alignment=2 stride=2 num_extra_inhabitants=0 bitwise_takable=1
// CHECK32-NEXT:                       (field name=_value offset=0
// CHECK32-NEXT:                         (builtin size=2 alignment=2 stride=2 num_extra_inhabitants=0 bitwise_takable=1))))))))))
// CHECK32-NEXT:       (case name=b index=1 offset=0
// CHECK32-NEXT:         (struct size=4 alignment=4 stride=4 num_extra_inhabitants=4096 bitwise_takable=1
// CHECK32-NEXT:           (field name=_buffer offset=0
// CHECK32-NEXT:             (struct size=4 alignment=4 stride=4 num_extra_inhabitants=4096 bitwise_takable=1
// CHECK32-NEXT:               (field name=_storage offset=0
// CHECK32-NEXT:                 (struct size=4 alignment=4 stride=4 num_extra_inhabitants=4096 bitwise_takable=1
// CHECK32-NEXT:                   (field name=rawValue offset=0
// CHECK32-NEXT:                     (builtin size=4 alignment=4 stride=4 num_extra_inhabitants=4096 bitwise_takable=1))))))))
// CHECK32-NEXT:       (case name=c index=2 offset=0
// CHECK32-NEXT:         (struct size=4 alignment=4 stride=4 num_extra_inhabitants=4096 bitwise_takable=1
// CHECK32-NEXT:           (field name=_variant offset=0
// CHECK32-NEXT:             (struct size=4 alignment=4 stride=4 num_extra_inhabitants=4096 bitwise_takable=1
// CHECK32-NEXT:               (field name=object offset=0
// CHECK32-NEXT:                 (struct size=4 alignment=4 stride=4 num_extra_inhabitants=4096 bitwise_takable=1
// CHECK32-NEXT:                   (field name=rawValue offset=0
// CHECK32-NEXT:                     (builtin size=4 alignment=4 stride=4 num_extra_inhabitants=4096 bitwise_takable=1))))))))
// CHECK32-NEXT:       (case name=extra index=3))))

reflect(enum: OneCaseNoPayload.only)

// CHECKALL: Reflecting an enum.
// CHECKALL-NEXT: Instance pointer in child address space: 0x{{[0-9a-fA-F]+}}
// CHECKALL-NEXT: Type reference:
// CHECKALL-NEXT: (enum reflect_Enum_values.OneCaseNoPayload)

// CHECKALL: Type info:
// CHECKALL-NEXT: (no_payload_enum size=0 alignment=1 stride=1 num_extra_inhabitants=0 bitwise_takable=1
// CHECKALL-NEXT:   (case name=only index=0))

// CHECKALL: Enum value:
// CHECKALL-NEXT: (enum_value name=only index=0)

reflect(enum: ManyCasesNoPayload.b)
// CHECKALL: Reflecting an enum.
// CHECKALL-NEXT: Instance pointer in child address space: 0x{{[0-9a-fA-F]+}}
// CHECKALL-NEXT: Type reference:
// CHECKALL-NEXT: (enum reflect_Enum_values.ManyCasesNoPayload)

// CHECKALL: Type info:
// CHECKALL-NEXT: (no_payload_enum size=1 alignment=1 stride=1 num_extra_inhabitants=252 bitwise_takable=1
// CHECKALL-NEXT:   (case name=a index=0)
// CHECKALL-NEXT:   (case name=b index=1)
// CHECKALL-NEXT:   (case name=c index=2)
// CHECKALL-NEXT:   (case name=d index=3))

// CHECKALL: Enum value:
// CHECKALL-NEXT: (enum_value name=b index=1)

reflect(enum: VastNumberOfCasesNoPayload.option12)
// CHECKALL: Reflecting an enum.
// CHECKALL-NEXT: Instance pointer in child address space: 0x{{[0-9a-fA-F]+}}
// CHECKALL-NEXT: Type reference:
// CHECKALL-NEXT: (enum reflect_Enum_values.VastNumberOfCasesNoPayload)

// CHECKALL: Type info:
// CHECKALL-NEXT: (no_payload_enum size=2 alignment=2 stride=2 num_extra_inhabitants=65278 bitwise_takable=1
// CHECKALL-NEXT:   (case name=option0 index=0)
// CHECKALL-NEXT:   (case name=option1 index=1)
// CHECKALL-NEXT:   (case name=option2 index=2)
// CHECKALL-NEXT:   (case name=option3 index=3)
// CHECKALL-NEXT:   (case name=option4 index=4)
// CHECKALL-NEXT:   (case name=option5 index=5)
// CHECKALL-NEXT:   (case name=option6 index=6)
// CHECKALL-NEXT:   (case name=option7 index=7)
// CHECKALL-NEXT:   (case name=option8 index=8)
// CHECKALL-NEXT:   (case name=option9 index=9)
// CHECKALL-NEXT:   (case name=option10 index=10)
// CHECKALL-NEXT:   (case name=option11 index=11)
// CHECKALL-NEXT:   (case name=option12 index=12)
// CHECKALL-NEXT:   (case name=option13 index=13)
// CHECKALL-NEXT:   (case name=option14 index=14)
// CHECKALL-NEXT:   (case name=option15 index=15)
// CHECKALL-NEXT:   (case name=option16 index=16)
// CHECKALL-NEXT:   (case name=option17 index=17)
// CHECKALL-NEXT:   (case name=option18 index=18)
// CHECKALL-NEXT:   (case name=option19 index=19)
// CHECKALL-NEXT:   (case name=option20 index=20)
// CHECKALL-NEXT:   (case name=option21 index=21)
// CHECKALL-NEXT:   (case name=option22 index=22)
// CHECKALL-NEXT:   (case name=option23 index=23)
// CHECKALL-NEXT:   (case name=option24 index=24)
// CHECKALL-NEXT:   (case name=option25 index=25)
// CHECKALL-NEXT:   (case name=option26 index=26)
// CHECKALL-NEXT:   (case name=option27 index=27)
// CHECKALL-NEXT:   (case name=option28 index=28)
// CHECKALL-NEXT:   (case name=option29 index=29)
// CHECKALL-NEXT:   (case name=option30 index=30)
// CHECKALL-NEXT:   (case name=option31 index=31)
// CHECKALL-NEXT:   (case name=option32 index=32)
// CHECKALL-NEXT:   (case name=option33 index=33)
// CHECKALL-NEXT:   (case name=option34 index=34)
// CHECKALL-NEXT:   (case name=option35 index=35)
// CHECKALL-NEXT:   (case name=option36 index=36)
// CHECKALL-NEXT:   (case name=option37 index=37)
// CHECKALL-NEXT:   (case name=option38 index=38)
// CHECKALL-NEXT:   (case name=option39 index=39)
// CHECKALL-NEXT:   (case name=option40 index=40)
// CHECKALL-NEXT:   (case name=option41 index=41)
// CHECKALL-NEXT:   (case name=option42 index=42)
// CHECKALL-NEXT:   (case name=option43 index=43)
// CHECKALL-NEXT:   (case name=option44 index=44)
// CHECKALL-NEXT:   (case name=option45 index=45)
// CHECKALL-NEXT:   (case name=option46 index=46)
// CHECKALL-NEXT:   (case name=option47 index=47)
// CHECKALL-NEXT:   (case name=option48 index=48)
// CHECKALL-NEXT:   (case name=option49 index=49)
// CHECKALL-NEXT:   (case name=option50 index=50)
// CHECKALL-NEXT:   (case name=option51 index=51)
// CHECKALL-NEXT:   (case name=option52 index=52)
// CHECKALL-NEXT:   (case name=option53 index=53)
// CHECKALL-NEXT:   (case name=option54 index=54)
// CHECKALL-NEXT:   (case name=option55 index=55)
// CHECKALL-NEXT:   (case name=option56 index=56)
// CHECKALL-NEXT:   (case name=option57 index=57)
// CHECKALL-NEXT:   (case name=option58 index=58)
// CHECKALL-NEXT:   (case name=option59 index=59)
// CHECKALL-NEXT:   (case name=option60 index=60)
// CHECKALL-NEXT:   (case name=option61 index=61)
// CHECKALL-NEXT:   (case name=option62 index=62)
// CHECKALL-NEXT:   (case name=option63 index=63)
// CHECKALL-NEXT:   (case name=option64 index=64)
// CHECKALL-NEXT:   (case name=option65 index=65)
// CHECKALL-NEXT:   (case name=option66 index=66)
// CHECKALL-NEXT:   (case name=option67 index=67)
// CHECKALL-NEXT:   (case name=option68 index=68)
// CHECKALL-NEXT:   (case name=option69 index=69)
// CHECKALL-NEXT:   (case name=option70 index=70)
// CHECKALL-NEXT:   (case name=option71 index=71)
// CHECKALL-NEXT:   (case name=option72 index=72)
// CHECKALL-NEXT:   (case name=option73 index=73)
// CHECKALL-NEXT:   (case name=option74 index=74)
// CHECKALL-NEXT:   (case name=option75 index=75)
// CHECKALL-NEXT:   (case name=option76 index=76)
// CHECKALL-NEXT:   (case name=option77 index=77)
// CHECKALL-NEXT:   (case name=option78 index=78)
// CHECKALL-NEXT:   (case name=option79 index=79)
// CHECKALL-NEXT:   (case name=option80 index=80)
// CHECKALL-NEXT:   (case name=option81 index=81)
// CHECKALL-NEXT:   (case name=option82 index=82)
// CHECKALL-NEXT:   (case name=option83 index=83)
// CHECKALL-NEXT:   (case name=option84 index=84)
// CHECKALL-NEXT:   (case name=option85 index=85)
// CHECKALL-NEXT:   (case name=option86 index=86)
// CHECKALL-NEXT:   (case name=option87 index=87)
// CHECKALL-NEXT:   (case name=option88 index=88)
// CHECKALL-NEXT:   (case name=option89 index=89)
// CHECKALL-NEXT:   (case name=option90 index=90)
// CHECKALL-NEXT:   (case name=option91 index=91)
// CHECKALL-NEXT:   (case name=option92 index=92)
// CHECKALL-NEXT:   (case name=option93 index=93)
// CHECKALL-NEXT:   (case name=option94 index=94)
// CHECKALL-NEXT:   (case name=option95 index=95)
// CHECKALL-NEXT:   (case name=option96 index=96)
// CHECKALL-NEXT:   (case name=option97 index=97)
// CHECKALL-NEXT:   (case name=option98 index=98)
// CHECKALL-NEXT:   (case name=option99 index=99)
// CHECKALL-NEXT:   (case name=option100 index=100)
// CHECKALL-NEXT:   (case name=option101 index=101)
// CHECKALL-NEXT:   (case name=option102 index=102)
// CHECKALL-NEXT:   (case name=option103 index=103)
// CHECKALL-NEXT:   (case name=option104 index=104)
// CHECKALL-NEXT:   (case name=option105 index=105)
// CHECKALL-NEXT:   (case name=option106 index=106)
// CHECKALL-NEXT:   (case name=option107 index=107)
// CHECKALL-NEXT:   (case name=option108 index=108)
// CHECKALL-NEXT:   (case name=option109 index=109)
// CHECKALL-NEXT:   (case name=option110 index=110)
// CHECKALL-NEXT:   (case name=option111 index=111)
// CHECKALL-NEXT:   (case name=option112 index=112)
// CHECKALL-NEXT:   (case name=option113 index=113)
// CHECKALL-NEXT:   (case name=option114 index=114)
// CHECKALL-NEXT:   (case name=option115 index=115)
// CHECKALL-NEXT:   (case name=option116 index=116)
// CHECKALL-NEXT:   (case name=option117 index=117)
// CHECKALL-NEXT:   (case name=option118 index=118)
// CHECKALL-NEXT:   (case name=option119 index=119)
// CHECKALL-NEXT:   (case name=option120 index=120)
// CHECKALL-NEXT:   (case name=option121 index=121)
// CHECKALL-NEXT:   (case name=option122 index=122)
// CHECKALL-NEXT:   (case name=option123 index=123)
// CHECKALL-NEXT:   (case name=option124 index=124)
// CHECKALL-NEXT:   (case name=option125 index=125)
// CHECKALL-NEXT:   (case name=option126 index=126)
// CHECKALL-NEXT:   (case name=option127 index=127)
// CHECKALL-NEXT:   (case name=option128 index=128)
// CHECKALL-NEXT:   (case name=option129 index=129)
// CHECKALL-NEXT:   (case name=option130 index=130)
// CHECKALL-NEXT:   (case name=option131 index=131)
// CHECKALL-NEXT:   (case name=option132 index=132)
// CHECKALL-NEXT:   (case name=option133 index=133)
// CHECKALL-NEXT:   (case name=option134 index=134)
// CHECKALL-NEXT:   (case name=option135 index=135)
// CHECKALL-NEXT:   (case name=option136 index=136)
// CHECKALL-NEXT:   (case name=option137 index=137)
// CHECKALL-NEXT:   (case name=option138 index=138)
// CHECKALL-NEXT:   (case name=option139 index=139)
// CHECKALL-NEXT:   (case name=option140 index=140)
// CHECKALL-NEXT:   (case name=option141 index=141)
// CHECKALL-NEXT:   (case name=option142 index=142)
// CHECKALL-NEXT:   (case name=option143 index=143)
// CHECKALL-NEXT:   (case name=option144 index=144)
// CHECKALL-NEXT:   (case name=option145 index=145)
// CHECKALL-NEXT:   (case name=option146 index=146)
// CHECKALL-NEXT:   (case name=option147 index=147)
// CHECKALL-NEXT:   (case name=option148 index=148)
// CHECKALL-NEXT:   (case name=option149 index=149)
// CHECKALL-NEXT:   (case name=option150 index=150)
// CHECKALL-NEXT:   (case name=option151 index=151)
// CHECKALL-NEXT:   (case name=option152 index=152)
// CHECKALL-NEXT:   (case name=option153 index=153)
// CHECKALL-NEXT:   (case name=option154 index=154)
// CHECKALL-NEXT:   (case name=option155 index=155)
// CHECKALL-NEXT:   (case name=option156 index=156)
// CHECKALL-NEXT:   (case name=option157 index=157)
// CHECKALL-NEXT:   (case name=option158 index=158)
// CHECKALL-NEXT:   (case name=option159 index=159)
// CHECKALL-NEXT:   (case name=option160 index=160)
// CHECKALL-NEXT:   (case name=option161 index=161)
// CHECKALL-NEXT:   (case name=option162 index=162)
// CHECKALL-NEXT:   (case name=option163 index=163)
// CHECKALL-NEXT:   (case name=option164 index=164)
// CHECKALL-NEXT:   (case name=option165 index=165)
// CHECKALL-NEXT:   (case name=option166 index=166)
// CHECKALL-NEXT:   (case name=option167 index=167)
// CHECKALL-NEXT:   (case name=option168 index=168)
// CHECKALL-NEXT:   (case name=option169 index=169)
// CHECKALL-NEXT:   (case name=option170 index=170)
// CHECKALL-NEXT:   (case name=option171 index=171)
// CHECKALL-NEXT:   (case name=option172 index=172)
// CHECKALL-NEXT:   (case name=option173 index=173)
// CHECKALL-NEXT:   (case name=option174 index=174)
// CHECKALL-NEXT:   (case name=option175 index=175)
// CHECKALL-NEXT:   (case name=option176 index=176)
// CHECKALL-NEXT:   (case name=option177 index=177)
// CHECKALL-NEXT:   (case name=option178 index=178)
// CHECKALL-NEXT:   (case name=option179 index=179)
// CHECKALL-NEXT:   (case name=option180 index=180)
// CHECKALL-NEXT:   (case name=option181 index=181)
// CHECKALL-NEXT:   (case name=option182 index=182)
// CHECKALL-NEXT:   (case name=option183 index=183)
// CHECKALL-NEXT:   (case name=option184 index=184)
// CHECKALL-NEXT:   (case name=option185 index=185)
// CHECKALL-NEXT:   (case name=option186 index=186)
// CHECKALL-NEXT:   (case name=option187 index=187)
// CHECKALL-NEXT:   (case name=option188 index=188)
// CHECKALL-NEXT:   (case name=option189 index=189)
// CHECKALL-NEXT:   (case name=option190 index=190)
// CHECKALL-NEXT:   (case name=option191 index=191)
// CHECKALL-NEXT:   (case name=option192 index=192)
// CHECKALL-NEXT:   (case name=option193 index=193)
// CHECKALL-NEXT:   (case name=option194 index=194)
// CHECKALL-NEXT:   (case name=option195 index=195)
// CHECKALL-NEXT:   (case name=option196 index=196)
// CHECKALL-NEXT:   (case name=option197 index=197)
// CHECKALL-NEXT:   (case name=option198 index=198)
// CHECKALL-NEXT:   (case name=option199 index=199)
// CHECKALL-NEXT:   (case name=option200 index=200)
// CHECKALL-NEXT:   (case name=option201 index=201)
// CHECKALL-NEXT:   (case name=option202 index=202)
// CHECKALL-NEXT:   (case name=option203 index=203)
// CHECKALL-NEXT:   (case name=option204 index=204)
// CHECKALL-NEXT:   (case name=option205 index=205)
// CHECKALL-NEXT:   (case name=option206 index=206)
// CHECKALL-NEXT:   (case name=option207 index=207)
// CHECKALL-NEXT:   (case name=option208 index=208)
// CHECKALL-NEXT:   (case name=option209 index=209)
// CHECKALL-NEXT:   (case name=option210 index=210)
// CHECKALL-NEXT:   (case name=option211 index=211)
// CHECKALL-NEXT:   (case name=option212 index=212)
// CHECKALL-NEXT:   (case name=option213 index=213)
// CHECKALL-NEXT:   (case name=option214 index=214)
// CHECKALL-NEXT:   (case name=option215 index=215)
// CHECKALL-NEXT:   (case name=option216 index=216)
// CHECKALL-NEXT:   (case name=option217 index=217)
// CHECKALL-NEXT:   (case name=option218 index=218)
// CHECKALL-NEXT:   (case name=option219 index=219)
// CHECKALL-NEXT:   (case name=option220 index=220)
// CHECKALL-NEXT:   (case name=option221 index=221)
// CHECKALL-NEXT:   (case name=option222 index=222)
// CHECKALL-NEXT:   (case name=option223 index=223)
// CHECKALL-NEXT:   (case name=option224 index=224)
// CHECKALL-NEXT:   (case name=option225 index=225)
// CHECKALL-NEXT:   (case name=option226 index=226)
// CHECKALL-NEXT:   (case name=option227 index=227)
// CHECKALL-NEXT:   (case name=option228 index=228)
// CHECKALL-NEXT:   (case name=option229 index=229)
// CHECKALL-NEXT:   (case name=option230 index=230)
// CHECKALL-NEXT:   (case name=option231 index=231)
// CHECKALL-NEXT:   (case name=option232 index=232)
// CHECKALL-NEXT:   (case name=option233 index=233)
// CHECKALL-NEXT:   (case name=option234 index=234)
// CHECKALL-NEXT:   (case name=option235 index=235)
// CHECKALL-NEXT:   (case name=option236 index=236)
// CHECKALL-NEXT:   (case name=option237 index=237)
// CHECKALL-NEXT:   (case name=option238 index=238)
// CHECKALL-NEXT:   (case name=option239 index=239)
// CHECKALL-NEXT:   (case name=option240 index=240)
// CHECKALL-NEXT:   (case name=option241 index=241)
// CHECKALL-NEXT:   (case name=option242 index=242)
// CHECKALL-NEXT:   (case name=option243 index=243)
// CHECKALL-NEXT:   (case name=option244 index=244)
// CHECKALL-NEXT:   (case name=option245 index=245)
// CHECKALL-NEXT:   (case name=option246 index=246)
// CHECKALL-NEXT:   (case name=option247 index=247)
// CHECKALL-NEXT:   (case name=option248 index=248)
// CHECKALL-NEXT:   (case name=option249 index=249)
// CHECKALL-NEXT:   (case name=option250 index=250)
// CHECKALL-NEXT:   (case name=option251 index=251)
// CHECKALL-NEXT:   (case name=option252 index=252)
// CHECKALL-NEXT:   (case name=option253 index=253)
// CHECKALL-NEXT:   (case name=option254 index=254)
// CHECKALL-NEXT:   (case name=option255 index=255)
// CHECKALL-NEXT:   (case name=option256 index=256)
// CHECKALL-NEXT:   (case name=option257 index=257))

// CHECKALL: Enum value:
// CHECKALL-NEXT: (enum_value name=option12 index=12)

reflect(enum: VastNumberOfCasesNoPayload.option256)

// CHECKALL: Reflecting an enum.
// CHECKALL-NEXT: Instance pointer in child address space: 0x{{[0-9a-fA-F]+}}
// CHECKALL-NEXT: Type reference:
// CHECKALL-NEXT: (enum reflect_Enum_values.VastNumberOfCasesNoPayload)

// CHECKALL: Type info:
// CHECKALL-NEXT: (no_payload_enum size=2 alignment=2 stride=2 num_extra_inhabitants=65278 bitwise_takable=1
// CHECKALL-NEXT:   (case name=option0 index=0)
// CHECKALL-NEXT:   (case name=option1 index=1)
// CHECKALL-NEXT:   (case name=option2 index=2)
// CHECKALL-NEXT:   (case name=option3 index=3)
// CHECKALL-NEXT:   (case name=option4 index=4)
// CHECKALL-NEXT:   (case name=option5 index=5)
// CHECKALL-NEXT:   (case name=option6 index=6)
// CHECKALL-NEXT:   (case name=option7 index=7)
// CHECKALL-NEXT:   (case name=option8 index=8)
// CHECKALL-NEXT:   (case name=option9 index=9)
// CHECKALL-NEXT:   (case name=option10 index=10)
// CHECKALL-NEXT:   (case name=option11 index=11)
// CHECKALL-NEXT:   (case name=option12 index=12)
// CHECKALL-NEXT:   (case name=option13 index=13)
// CHECKALL-NEXT:   (case name=option14 index=14)
// CHECKALL-NEXT:   (case name=option15 index=15)
// CHECKALL-NEXT:   (case name=option16 index=16)
// CHECKALL-NEXT:   (case name=option17 index=17)
// CHECKALL-NEXT:   (case name=option18 index=18)
// CHECKALL-NEXT:   (case name=option19 index=19)
// CHECKALL-NEXT:   (case name=option20 index=20)
// CHECKALL-NEXT:   (case name=option21 index=21)
// CHECKALL-NEXT:   (case name=option22 index=22)
// CHECKALL-NEXT:   (case name=option23 index=23)
// CHECKALL-NEXT:   (case name=option24 index=24)
// CHECKALL-NEXT:   (case name=option25 index=25)
// CHECKALL-NEXT:   (case name=option26 index=26)
// CHECKALL-NEXT:   (case name=option27 index=27)
// CHECKALL-NEXT:   (case name=option28 index=28)
// CHECKALL-NEXT:   (case name=option29 index=29)
// CHECKALL-NEXT:   (case name=option30 index=30)
// CHECKALL-NEXT:   (case name=option31 index=31)
// CHECKALL-NEXT:   (case name=option32 index=32)
// CHECKALL-NEXT:   (case name=option33 index=33)
// CHECKALL-NEXT:   (case name=option34 index=34)
// CHECKALL-NEXT:   (case name=option35 index=35)
// CHECKALL-NEXT:   (case name=option36 index=36)
// CHECKALL-NEXT:   (case name=option37 index=37)
// CHECKALL-NEXT:   (case name=option38 index=38)
// CHECKALL-NEXT:   (case name=option39 index=39)
// CHECKALL-NEXT:   (case name=option40 index=40)
// CHECKALL-NEXT:   (case name=option41 index=41)
// CHECKALL-NEXT:   (case name=option42 index=42)
// CHECKALL-NEXT:   (case name=option43 index=43)
// CHECKALL-NEXT:   (case name=option44 index=44)
// CHECKALL-NEXT:   (case name=option45 index=45)
// CHECKALL-NEXT:   (case name=option46 index=46)
// CHECKALL-NEXT:   (case name=option47 index=47)
// CHECKALL-NEXT:   (case name=option48 index=48)
// CHECKALL-NEXT:   (case name=option49 index=49)
// CHECKALL-NEXT:   (case name=option50 index=50)
// CHECKALL-NEXT:   (case name=option51 index=51)
// CHECKALL-NEXT:   (case name=option52 index=52)
// CHECKALL-NEXT:   (case name=option53 index=53)
// CHECKALL-NEXT:   (case name=option54 index=54)
// CHECKALL-NEXT:   (case name=option55 index=55)
// CHECKALL-NEXT:   (case name=option56 index=56)
// CHECKALL-NEXT:   (case name=option57 index=57)
// CHECKALL-NEXT:   (case name=option58 index=58)
// CHECKALL-NEXT:   (case name=option59 index=59)
// CHECKALL-NEXT:   (case name=option60 index=60)
// CHECKALL-NEXT:   (case name=option61 index=61)
// CHECKALL-NEXT:   (case name=option62 index=62)
// CHECKALL-NEXT:   (case name=option63 index=63)
// CHECKALL-NEXT:   (case name=option64 index=64)
// CHECKALL-NEXT:   (case name=option65 index=65)
// CHECKALL-NEXT:   (case name=option66 index=66)
// CHECKALL-NEXT:   (case name=option67 index=67)
// CHECKALL-NEXT:   (case name=option68 index=68)
// CHECKALL-NEXT:   (case name=option69 index=69)
// CHECKALL-NEXT:   (case name=option70 index=70)
// CHECKALL-NEXT:   (case name=option71 index=71)
// CHECKALL-NEXT:   (case name=option72 index=72)
// CHECKALL-NEXT:   (case name=option73 index=73)
// CHECKALL-NEXT:   (case name=option74 index=74)
// CHECKALL-NEXT:   (case name=option75 index=75)
// CHECKALL-NEXT:   (case name=option76 index=76)
// CHECKALL-NEXT:   (case name=option77 index=77)
// CHECKALL-NEXT:   (case name=option78 index=78)
// CHECKALL-NEXT:   (case name=option79 index=79)
// CHECKALL-NEXT:   (case name=option80 index=80)
// CHECKALL-NEXT:   (case name=option81 index=81)
// CHECKALL-NEXT:   (case name=option82 index=82)
// CHECKALL-NEXT:   (case name=option83 index=83)
// CHECKALL-NEXT:   (case name=option84 index=84)
// CHECKALL-NEXT:   (case name=option85 index=85)
// CHECKALL-NEXT:   (case name=option86 index=86)
// CHECKALL-NEXT:   (case name=option87 index=87)
// CHECKALL-NEXT:   (case name=option88 index=88)
// CHECKALL-NEXT:   (case name=option89 index=89)
// CHECKALL-NEXT:   (case name=option90 index=90)
// CHECKALL-NEXT:   (case name=option91 index=91)
// CHECKALL-NEXT:   (case name=option92 index=92)
// CHECKALL-NEXT:   (case name=option93 index=93)
// CHECKALL-NEXT:   (case name=option94 index=94)
// CHECKALL-NEXT:   (case name=option95 index=95)
// CHECKALL-NEXT:   (case name=option96 index=96)
// CHECKALL-NEXT:   (case name=option97 index=97)
// CHECKALL-NEXT:   (case name=option98 index=98)
// CHECKALL-NEXT:   (case name=option99 index=99)
// CHECKALL-NEXT:   (case name=option100 index=100)
// CHECKALL-NEXT:   (case name=option101 index=101)
// CHECKALL-NEXT:   (case name=option102 index=102)
// CHECKALL-NEXT:   (case name=option103 index=103)
// CHECKALL-NEXT:   (case name=option104 index=104)
// CHECKALL-NEXT:   (case name=option105 index=105)
// CHECKALL-NEXT:   (case name=option106 index=106)
// CHECKALL-NEXT:   (case name=option107 index=107)
// CHECKALL-NEXT:   (case name=option108 index=108)
// CHECKALL-NEXT:   (case name=option109 index=109)
// CHECKALL-NEXT:   (case name=option110 index=110)
// CHECKALL-NEXT:   (case name=option111 index=111)
// CHECKALL-NEXT:   (case name=option112 index=112)
// CHECKALL-NEXT:   (case name=option113 index=113)
// CHECKALL-NEXT:   (case name=option114 index=114)
// CHECKALL-NEXT:   (case name=option115 index=115)
// CHECKALL-NEXT:   (case name=option116 index=116)
// CHECKALL-NEXT:   (case name=option117 index=117)
// CHECKALL-NEXT:   (case name=option118 index=118)
// CHECKALL-NEXT:   (case name=option119 index=119)
// CHECKALL-NEXT:   (case name=option120 index=120)
// CHECKALL-NEXT:   (case name=option121 index=121)
// CHECKALL-NEXT:   (case name=option122 index=122)
// CHECKALL-NEXT:   (case name=option123 index=123)
// CHECKALL-NEXT:   (case name=option124 index=124)
// CHECKALL-NEXT:   (case name=option125 index=125)
// CHECKALL-NEXT:   (case name=option126 index=126)
// CHECKALL-NEXT:   (case name=option127 index=127)
// CHECKALL-NEXT:   (case name=option128 index=128)
// CHECKALL-NEXT:   (case name=option129 index=129)
// CHECKALL-NEXT:   (case name=option130 index=130)
// CHECKALL-NEXT:   (case name=option131 index=131)
// CHECKALL-NEXT:   (case name=option132 index=132)
// CHECKALL-NEXT:   (case name=option133 index=133)
// CHECKALL-NEXT:   (case name=option134 index=134)
// CHECKALL-NEXT:   (case name=option135 index=135)
// CHECKALL-NEXT:   (case name=option136 index=136)
// CHECKALL-NEXT:   (case name=option137 index=137)
// CHECKALL-NEXT:   (case name=option138 index=138)
// CHECKALL-NEXT:   (case name=option139 index=139)
// CHECKALL-NEXT:   (case name=option140 index=140)
// CHECKALL-NEXT:   (case name=option141 index=141)
// CHECKALL-NEXT:   (case name=option142 index=142)
// CHECKALL-NEXT:   (case name=option143 index=143)
// CHECKALL-NEXT:   (case name=option144 index=144)
// CHECKALL-NEXT:   (case name=option145 index=145)
// CHECKALL-NEXT:   (case name=option146 index=146)
// CHECKALL-NEXT:   (case name=option147 index=147)
// CHECKALL-NEXT:   (case name=option148 index=148)
// CHECKALL-NEXT:   (case name=option149 index=149)
// CHECKALL-NEXT:   (case name=option150 index=150)
// CHECKALL-NEXT:   (case name=option151 index=151)
// CHECKALL-NEXT:   (case name=option152 index=152)
// CHECKALL-NEXT:   (case name=option153 index=153)
// CHECKALL-NEXT:   (case name=option154 index=154)
// CHECKALL-NEXT:   (case name=option155 index=155)
// CHECKALL-NEXT:   (case name=option156 index=156)
// CHECKALL-NEXT:   (case name=option157 index=157)
// CHECKALL-NEXT:   (case name=option158 index=158)
// CHECKALL-NEXT:   (case name=option159 index=159)
// CHECKALL-NEXT:   (case name=option160 index=160)
// CHECKALL-NEXT:   (case name=option161 index=161)
// CHECKALL-NEXT:   (case name=option162 index=162)
// CHECKALL-NEXT:   (case name=option163 index=163)
// CHECKALL-NEXT:   (case name=option164 index=164)
// CHECKALL-NEXT:   (case name=option165 index=165)
// CHECKALL-NEXT:   (case name=option166 index=166)
// CHECKALL-NEXT:   (case name=option167 index=167)
// CHECKALL-NEXT:   (case name=option168 index=168)
// CHECKALL-NEXT:   (case name=option169 index=169)
// CHECKALL-NEXT:   (case name=option170 index=170)
// CHECKALL-NEXT:   (case name=option171 index=171)
// CHECKALL-NEXT:   (case name=option172 index=172)
// CHECKALL-NEXT:   (case name=option173 index=173)
// CHECKALL-NEXT:   (case name=option174 index=174)
// CHECKALL-NEXT:   (case name=option175 index=175)
// CHECKALL-NEXT:   (case name=option176 index=176)
// CHECKALL-NEXT:   (case name=option177 index=177)
// CHECKALL-NEXT:   (case name=option178 index=178)
// CHECKALL-NEXT:   (case name=option179 index=179)
// CHECKALL-NEXT:   (case name=option180 index=180)
// CHECKALL-NEXT:   (case name=option181 index=181)
// CHECKALL-NEXT:   (case name=option182 index=182)
// CHECKALL-NEXT:   (case name=option183 index=183)
// CHECKALL-NEXT:   (case name=option184 index=184)
// CHECKALL-NEXT:   (case name=option185 index=185)
// CHECKALL-NEXT:   (case name=option186 index=186)
// CHECKALL-NEXT:   (case name=option187 index=187)
// CHECKALL-NEXT:   (case name=option188 index=188)
// CHECKALL-NEXT:   (case name=option189 index=189)
// CHECKALL-NEXT:   (case name=option190 index=190)
// CHECKALL-NEXT:   (case name=option191 index=191)
// CHECKALL-NEXT:   (case name=option192 index=192)
// CHECKALL-NEXT:   (case name=option193 index=193)
// CHECKALL-NEXT:   (case name=option194 index=194)
// CHECKALL-NEXT:   (case name=option195 index=195)
// CHECKALL-NEXT:   (case name=option196 index=196)
// CHECKALL-NEXT:   (case name=option197 index=197)
// CHECKALL-NEXT:   (case name=option198 index=198)
// CHECKALL-NEXT:   (case name=option199 index=199)
// CHECKALL-NEXT:   (case name=option200 index=200)
// CHECKALL-NEXT:   (case name=option201 index=201)
// CHECKALL-NEXT:   (case name=option202 index=202)
// CHECKALL-NEXT:   (case name=option203 index=203)
// CHECKALL-NEXT:   (case name=option204 index=204)
// CHECKALL-NEXT:   (case name=option205 index=205)
// CHECKALL-NEXT:   (case name=option206 index=206)
// CHECKALL-NEXT:   (case name=option207 index=207)
// CHECKALL-NEXT:   (case name=option208 index=208)
// CHECKALL-NEXT:   (case name=option209 index=209)
// CHECKALL-NEXT:   (case name=option210 index=210)
// CHECKALL-NEXT:   (case name=option211 index=211)
// CHECKALL-NEXT:   (case name=option212 index=212)
// CHECKALL-NEXT:   (case name=option213 index=213)
// CHECKALL-NEXT:   (case name=option214 index=214)
// CHECKALL-NEXT:   (case name=option215 index=215)
// CHECKALL-NEXT:   (case name=option216 index=216)
// CHECKALL-NEXT:   (case name=option217 index=217)
// CHECKALL-NEXT:   (case name=option218 index=218)
// CHECKALL-NEXT:   (case name=option219 index=219)
// CHECKALL-NEXT:   (case name=option220 index=220)
// CHECKALL-NEXT:   (case name=option221 index=221)
// CHECKALL-NEXT:   (case name=option222 index=222)
// CHECKALL-NEXT:   (case name=option223 index=223)
// CHECKALL-NEXT:   (case name=option224 index=224)
// CHECKALL-NEXT:   (case name=option225 index=225)
// CHECKALL-NEXT:   (case name=option226 index=226)
// CHECKALL-NEXT:   (case name=option227 index=227)
// CHECKALL-NEXT:   (case name=option228 index=228)
// CHECKALL-NEXT:   (case name=option229 index=229)
// CHECKALL-NEXT:   (case name=option230 index=230)
// CHECKALL-NEXT:   (case name=option231 index=231)
// CHECKALL-NEXT:   (case name=option232 index=232)
// CHECKALL-NEXT:   (case name=option233 index=233)
// CHECKALL-NEXT:   (case name=option234 index=234)
// CHECKALL-NEXT:   (case name=option235 index=235)
// CHECKALL-NEXT:   (case name=option236 index=236)
// CHECKALL-NEXT:   (case name=option237 index=237)
// CHECKALL-NEXT:   (case name=option238 index=238)
// CHECKALL-NEXT:   (case name=option239 index=239)
// CHECKALL-NEXT:   (case name=option240 index=240)
// CHECKALL-NEXT:   (case name=option241 index=241)
// CHECKALL-NEXT:   (case name=option242 index=242)
// CHECKALL-NEXT:   (case name=option243 index=243)
// CHECKALL-NEXT:   (case name=option244 index=244)
// CHECKALL-NEXT:   (case name=option245 index=245)
// CHECKALL-NEXT:   (case name=option246 index=246)
// CHECKALL-NEXT:   (case name=option247 index=247)
// CHECKALL-NEXT:   (case name=option248 index=248)
// CHECKALL-NEXT:   (case name=option249 index=249)
// CHECKALL-NEXT:   (case name=option250 index=250)
// CHECKALL-NEXT:   (case name=option251 index=251)
// CHECKALL-NEXT:   (case name=option252 index=252)
// CHECKALL-NEXT:   (case name=option253 index=253)
// CHECKALL-NEXT:   (case name=option254 index=254)
// CHECKALL-NEXT:   (case name=option255 index=255)
// CHECKALL-NEXT:   (case name=option256 index=256)
// CHECKALL-NEXT:   (case name=option257 index=257))

// CHECKALL: Enum value:
// CHECKALL-NEXT: (enum_value name=option256 index=256)

reflect(enum: ManyCasesOneIntPayload.payload(77))

// CHECKALL: Reflecting an enum.
// CHECKALL-NEXT: Instance pointer in child address space: 0x{{[0-9a-fA-F]+}}
// CHECKALL-NEXT: Type reference:
// CHECKALL-NEXT: (enum reflect_Enum_values.ManyCasesOneIntPayload)

// CHECKALL: Type info:
// CHECK64-NEXT: (single_payload_enum size=9 alignment=8 stride=16 num_extra_inhabitants=0 bitwise_takable=1
// CHECK64-NEXT:   (case name=payload index=0 offset=0
// CHECK64-NEXT:     (struct size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1
// CHECK64-NEXT:       (field name=_value offset=0
// CHECK64-NEXT:         (builtin size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1))))
// CHECK64-NEXT:   (case name=otherA index=1)
// CHECK64-NEXT:   (case name=otherB index=2)
// CHECK64-NEXT:   (case name=otherC index=3))

// CHECK32-NEXT: (single_payload_enum size=5 alignment=4 stride=8 num_extra_inhabitants=0 bitwise_takable=1
// CHECK32-NEXT:   (case name=payload index=0 offset=0
// CHECK32-NEXT:     (struct size=4 alignment=4 stride=4 num_extra_inhabitants=0 bitwise_takable=1
// CHECK32-NEXT:       (field name=_value offset=0
// CHECK32-NEXT:         (builtin size=4 alignment=4 stride=4 num_extra_inhabitants=0 bitwise_takable=1))))
// CHECK32-NEXT:   (case name=otherA index=1)
// CHECK32-NEXT:   (case name=otherB index=2)
// CHECK32-NEXT:   (case name=otherC index=3))

// CHECKALL: Enum value:
// CHECKALL-NEXT: (enum_value name=payload index=0
// CHECKALL-NEXT: (struct Swift.Int)
// CHECKALL-NEXT: )

reflect(enum: ManyCasesOneStringPayload.payload("hello, world"))

// CHECKALL: Reflecting an enum.
// CHECKALL-NEXT: Instance pointer in child address space: 0x{{[0-9a-fA-F]+}}
// CHECKALL-NEXT: Type reference:
// CHECKALL-NEXT: (enum reflect_Enum_values.ManyCasesOneStringPayload)

// CHECKALL: Type info:
// CHECK64-NEXT: (single_payload_enum size=16 alignment=8 stride=16 num_extra_inhabitants=[[#num_extra_inhabitants_64bit-3]] bitwise_takable=1
// CHECK64-NEXT:   (case name=payload index=0 offset=0
// CHECK64-NEXT:     (struct size=16 alignment=8 stride=16 num_extra_inhabitants=[[#num_extra_inhabitants_64bit]] bitwise_takable=1
// CHECK64-NEXT:       (field name=_guts offset=0
// CHECK64-NEXT:         (struct size=16 alignment=8 stride=16 num_extra_inhabitants=[[#num_extra_inhabitants_64bit]] bitwise_takable=1
// CHECK64-NEXT:           (field name=_object offset=0
// CHECK64-NEXT:             (struct size=16 alignment=8 stride=16 num_extra_inhabitants=[[#num_extra_inhabitants_64bit]] bitwise_takable=1
// CHECK64-NEXT:               (field name=_countAndFlagsBits offset=0
// CHECK64-NEXT:                 (struct size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1
// CHECK64-NEXT:                   (field name=_value offset=0
// CHECK64-NEXT:                     (builtin size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1))))
// CHECK64-NEXT:               (field name=_object offset=8
// CHECK64-NEXT:                 (builtin size=8 alignment=8 stride=8 num_extra_inhabitants=[[#num_extra_inhabitants_64bit]] bitwise_takable=1))))))))
// CHECK64-NEXT:   (case name=otherA index=1)
// CHECK64-NEXT:   (case name=otherB index=2)
// CHECK64-NEXT:   (case name=otherC index=3))

// CHECK32-NEXT: (single_payload_enum size=12 alignment=4 stride=12 num_extra_inhabitants=250 bitwise_takable=1
// CHECK32-NEXT:   (case name=payload index=0 offset=0
// CHECK32-NEXT:     (struct size=12 alignment=4 stride=12 num_extra_inhabitants=253 bitwise_takable=1
// CHECK32-NEXT:       (field name=_guts offset=0
// CHECK32-NEXT:         (struct size=12 alignment=4 stride=12 num_extra_inhabitants=253 bitwise_takable=1
// CHECK32-NEXT:           (field name=_object offset=0
// CHECK32-NEXT:             (struct size=12 alignment=4 stride=12 num_extra_inhabitants=253 bitwise_takable=1
// CHECK32-NEXT:               (field name=_count offset=0
// CHECK32-NEXT:                 (struct size=4 alignment=4 stride=4 num_extra_inhabitants=0 bitwise_takable=1
// CHECK32-NEXT:                   (field name=_value offset=0
// CHECK32-NEXT:                     (builtin size=4 alignment=4 stride=4 num_extra_inhabitants=0 bitwise_takable=1))))
// CHECK32-NEXT:               (field name=_variant offset=4
// CHECK32-NEXT:                 (multi_payload_enum size=5 alignment=4 stride=8 num_extra_inhabitants=253 bitwise_takable=1
// CHECK32-NEXT:                   (case name=immortal index=0 offset=0
// CHECK32-NEXT:                     (struct size=4 alignment=4 stride=4 num_extra_inhabitants=0 bitwise_takable=1
// CHECK32-NEXT:                       (field name=_value offset=0
// CHECK32-NEXT:                         (builtin size=4 alignment=4 stride=4 num_extra_inhabitants=0 bitwise_takable=1))))
// CHECK32-NEXT:                   (case name=native index=1 offset=0
// CHECK32-NEXT:                     (class_existential size=4 alignment=4 stride=4 num_extra_inhabitants=4096 bitwise_takable=1
// CHECK32-NEXT:                       (field name=object offset=0
// CHECK32-NEXT:                         (reference kind=strong refcounting=unknown))))
// CHECK32-NEXT:                   (case name=bridged index=2 offset=0
// CHECK32-NEXT:                     (class_existential size=4 alignment=4 stride=4 num_extra_inhabitants=4096 bitwise_takable=1
// CHECK32-NEXT:                       (field name=object offset=0
// CHECK32-NEXT:                         (reference kind=strong refcounting=unknown))))))
// CHECK32-NEXT:               (field name=_discriminator offset=9
// CHECK32-NEXT:                 (struct size=1 alignment=1 stride=1 num_extra_inhabitants=0 bitwise_takable=1
// CHECK32-NEXT:                   (field name=_value offset=0
// CHECK32-NEXT:                     (builtin size=1 alignment=1 stride=1 num_extra_inhabitants=0 bitwise_takable=1))))
// CHECK32-NEXT:               (field name=_flags offset=10
// CHECK32-NEXT:                 (struct size=2 alignment=2 stride=2 num_extra_inhabitants=0 bitwise_takable=1
// CHECK32-NEXT:                   (field name=_value offset=0
// CHECK32-NEXT:                     (builtin size=2 alignment=2 stride=2 num_extra_inhabitants=0 bitwise_takable=1))))))))))
// CHECK32-NEXT:   (case name=otherA index=1)
// CHECK32-NEXT:   (case name=otherB index=2)
// CHECK32-NEXT:   (case name=otherC index=3))

// CHECKALL: Enum value:
// CHECKALL-NEXT: (enum_value name=payload index=0
// CHECKALL-NEXT: (struct Swift.String)
// CHECKALL-NEXT: )

reflect(enum: ManyCasesOneStringPayload.otherB)

// CHECKALL: Reflecting an enum.
// CHECKALL-NEXT: Instance pointer in child address space: 0x{{[0-9a-fA-F]+}}
// CHECKALL-NEXT: Type reference:
// CHECKALL-NEXT: (enum reflect_Enum_values.ManyCasesOneStringPayload)

// CHECKALL: Type info:
// CHECK64-NEXT: (single_payload_enum size=16 alignment=8 stride=16 num_extra_inhabitants=[[#num_extra_inhabitants_64bit-3]] bitwise_takable=1
// CHECK64-NEXT:   (case name=payload index=0 offset=0
// CHECK64-NEXT:     (struct size=16 alignment=8 stride=16 num_extra_inhabitants=[[#num_extra_inhabitants_64bit]] bitwise_takable=1
// CHECK64-NEXT:       (field name=_guts offset=0
// CHECK64-NEXT:         (struct size=16 alignment=8 stride=16 num_extra_inhabitants=[[#num_extra_inhabitants_64bit]] bitwise_takable=1
// CHECK64-NEXT:           (field name=_object offset=0
// CHECK64-NEXT:             (struct size=16 alignment=8 stride=16 num_extra_inhabitants=[[#num_extra_inhabitants_64bit]] bitwise_takable=1
// CHECK64-NEXT:               (field name=_countAndFlagsBits offset=0
// CHECK64-NEXT:                 (struct size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1
// CHECK64-NEXT:                   (field name=_value offset=0
// CHECK64-NEXT:                     (builtin size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1))))
// CHECK64-NEXT:               (field name=_object offset=8
// CHECK64-NEXT:                 (builtin size=8 alignment=8 stride=8 num_extra_inhabitants=[[#num_extra_inhabitants_64bit]] bitwise_takable=1))))))))
// CHECK64-NEXT:   (case name=otherA index=1)
// CHECK64-NEXT:   (case name=otherB index=2)
// CHECK64-NEXT:   (case name=otherC index=3))

// CHECK32-NEXT: (single_payload_enum size=12 alignment=4 stride=12 num_extra_inhabitants=250 bitwise_takable=1
// CHECK32-NEXT:   (case name=payload index=0 offset=0
// CHECK32-NEXT:     (struct size=12 alignment=4 stride=12 num_extra_inhabitants=253 bitwise_takable=1
// CHECK32-NEXT:       (field name=_guts offset=0
// CHECK32-NEXT:         (struct size=12 alignment=4 stride=12 num_extra_inhabitants=253 bitwise_takable=1
// CHECK32-NEXT:           (field name=_object offset=0
// CHECK32-NEXT:             (struct size=12 alignment=4 stride=12 num_extra_inhabitants=253 bitwise_takable=1
// CHECK32-NEXT:               (field name=_count offset=0
// CHECK32-NEXT:                 (struct size=4 alignment=4 stride=4 num_extra_inhabitants=0 bitwise_takable=1
// CHECK32-NEXT:                   (field name=_value offset=0
// CHECK32-NEXT:                     (builtin size=4 alignment=4 stride=4 num_extra_inhabitants=0 bitwise_takable=1))))
// CHECK32-NEXT:               (field name=_variant offset=4
// CHECK32-NEXT:                 (multi_payload_enum size=5 alignment=4 stride=8 num_extra_inhabitants=253 bitwise_takable=1
// CHECK32-NEXT:                   (case name=immortal index=0 offset=0
// CHECK32-NEXT:                     (struct size=4 alignment=4 stride=4 num_extra_inhabitants=0 bitwise_takable=1
// CHECK32-NEXT:                       (field name=_value offset=0
// CHECK32-NEXT:                         (builtin size=4 alignment=4 stride=4 num_extra_inhabitants=0 bitwise_takable=1))))
// CHECK32-NEXT:                   (case name=native index=1 offset=0
// CHECK32-NEXT:                     (class_existential size=4 alignment=4 stride=4 num_extra_inhabitants=4096 bitwise_takable=1
// CHECK32-NEXT:                       (field name=object offset=0
// CHECK32-NEXT:                         (reference kind=strong refcounting=unknown))))
// CHECK32-NEXT:                   (case name=bridged index=2 offset=0
// CHECK32-NEXT:                     (class_existential size=4 alignment=4 stride=4 num_extra_inhabitants=4096 bitwise_takable=1
// CHECK32-NEXT:                       (field name=object offset=0
// CHECK32-NEXT:                         (reference kind=strong refcounting=unknown))))))
// CHECK32-NEXT:               (field name=_discriminator offset=9
// CHECK32-NEXT:                 (struct size=1 alignment=1 stride=1 num_extra_inhabitants=0 bitwise_takable=1
// CHECK32-NEXT:                   (field name=_value offset=0
// CHECK32-NEXT:                     (builtin size=1 alignment=1 stride=1 num_extra_inhabitants=0 bitwise_takable=1))))
// CHECK32-NEXT:               (field name=_flags offset=10
// CHECK32-NEXT:                 (struct size=2 alignment=2 stride=2 num_extra_inhabitants=0 bitwise_takable=1
// CHECK32-NEXT:                   (field name=_value offset=0
// CHECK32-NEXT:                     (builtin size=2 alignment=2 stride=2 num_extra_inhabitants=0 bitwise_takable=1))))))))))
// CHECK32-NEXT:   (case name=otherA index=1)
// CHECK32-NEXT:   (case name=otherB index=2)
// CHECK32-NEXT:   (case name=otherC index=3))


// CHECKALL: Enum value:
// CHECKALL-NEXT: (enum_value name=otherB index=2)

doneReflecting()

// CHECKALL: Done.

