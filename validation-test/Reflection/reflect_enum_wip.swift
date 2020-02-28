// RUN: %empty-directory(%t)
// RUN: %target-build-swift -lswiftSwiftReflectionTest %s -o %t/reflect_enum_wip
// RUN: %target-codesign %t/reflect_enum_wip

// RUN: %target-run %target-swift-reflection-test %t/reflect_enum_wip | tee /dev/stderr | %FileCheck %s --check-prefix=CHECK-%target-ptrsize --dump-input=fail

// REQUIRES: objc_interop
// REQUIRES: executable_test

import SwiftReflectionTest

enum OneCaseNoPayload {
  case only
}

class OneCaseNoPayloadC {
  var x = OneCaseNoPayload.only
  var y = 42
}
reflect(object: OneCaseNoPayloadC())
// CHECK-64: Reflecting an object.
// CHECK-64: Instance pointer in child address space: 0x{{[0-9a-fA-F]+}}
// CHECK-64: Type reference:
// CHECK-64: (class reflect_enum_wip.OneCaseNoPayloadC)

// CHECK-64: Type info:
// CHECK-64: (class_instance size=24 alignment=8 stride=24 num_extra_inhabitants=0 bitwise_takable=1
// CHECK-64:   (field name=x offset=16
// CHECK-64:     (no_payload_enum size=0 alignment=1 stride=1 num_extra_inhabitants=0 bitwise_takable=1
// CHECK-64:       (case name=only index=0)))
// CHECK-64:   (field name=y offset=16
// CHECK-64:     (struct size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1
// CHECK-64:       (field name=_value offset=0
// CHECK-64:         (builtin size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1)))))

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

// CHECK-64: Reflecting an object.
// CHECK-64: Instance pointer in child address space: 0x{{[0-9a-fA-F]+}}
// CHECK-64: Type reference:
// CHECK-64: (class reflect_enum_wip.ManyCasesNoPayloadC)

// CHECK-64: Type info:
// CHECK-64: (class_instance size=40 alignment=8 stride=40 num_extra_inhabitants=0 bitwise_takable=1
// CHECK-64:   (field name=a offset=16
// CHECK-64:     (no_payload_enum size=1 alignment=1 stride=1 num_extra_inhabitants=252 bitwise_takable=1
// CHECK-64:       (case name=a index=0)
// CHECK-64:       (case name=b index=1)
// CHECK-64:       (case name=c index=2)
// CHECK-64:       (case name=d index=3)))
// CHECK-64:   (field name=b offset=17
// CHECK-64:     (no_payload_enum size=1 alignment=1 stride=1 num_extra_inhabitants=252 bitwise_takable=1
// CHECK-64:       (case name=a index=0)
// CHECK-64:       (case name=b index=1)
// CHECK-64:       (case name=c index=2)
// CHECK-64:       (case name=d index=3)))
// CHECK-64:   (field name=c offset=18
// CHECK-64:     (no_payload_enum size=1 alignment=1 stride=1 num_extra_inhabitants=252 bitwise_takable=1
// CHECK-64:       (case name=a index=0)
// CHECK-64:       (case name=b index=1)
// CHECK-64:       (case name=c index=2)
// CHECK-64:       (case name=d index=3)))
// CHECK-64:   (field name=d offset=19
// CHECK-64:     (no_payload_enum size=1 alignment=1 stride=1 num_extra_inhabitants=252 bitwise_takable=1
// CHECK-64:       (case name=a index=0)
// CHECK-64:       (case name=b index=1)
// CHECK-64:       (case name=c index=2)
// CHECK-64:       (case name=d index=3)))
// CHECK-64:   (field name=s offset=24
// CHECK-64:     (struct size=16 alignment=8 stride=16 num_extra_inhabitants=2147483647 bitwise_takable=1
// CHECK-64:       (field name=_guts offset=0
// CHECK-64:         (struct size=16 alignment=8 stride=16 num_extra_inhabitants=2147483647 bitwise_takable=1
// CHECK-64:           (field name=_object offset=0
// CHECK-64:             (struct size=16 alignment=8 stride=16 num_extra_inhabitants=2147483647 bitwise_takable=1
// CHECK-64:               (field name=_countAndFlagsBits offset=0
// CHECK-64:                 (struct size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1
// CHECK-64:                   (field name=_value offset=0
// CHECK-64:                     (builtin size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1))))
// CHECK-64:               (field name=_object offset=8
// CHECK-64:                 (builtin size=8 alignment=8 stride=8 num_extra_inhabitants=2147483647 bitwise_takable=1)))))))))


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

// CHECK-64: Reflecting an object.
// CHECK-64: Instance pointer in child address space: 0x{{[0-9a-fA-F]+}}
// CHECK-64: Type reference:
// CHECK-64: (class reflect_enum_wip.ManyCasesOnePayloadC)

// CHECK-64: Type info:
// CHECK-64: (class_instance size=80 alignment=8 stride=80 num_extra_inhabitants=0 bitwise_takable=1
// CHECK-64:   (field name=payload offset=16
// CHECK-64:     (single_payload_enum size=16 alignment=8 stride=16 num_extra_inhabitants=2147483644 bitwise_takable=1
// CHECK-64:       (case name=payload index=0 offset=0
// CHECK-64:         (struct size=16 alignment=8 stride=16 num_extra_inhabitants=2147483647 bitwise_takable=1
// CHECK-64:           (field name=_guts offset=0
// CHECK-64:             (struct size=16 alignment=8 stride=16 num_extra_inhabitants=2147483647 bitwise_takable=1
// CHECK-64:               (field name=_object offset=0
// CHECK-64:                 (struct size=16 alignment=8 stride=16 num_extra_inhabitants=2147483647 bitwise_takable=1
// CHECK-64:                   (field name=_countAndFlagsBits offset=0
// CHECK-64:                     (struct size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1
// CHECK-64:                       (field name=_value offset=0
// CHECK-64:                         (builtin size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1))))
// CHECK-64:                   (field name=_object offset=8
// CHECK-64:                     (builtin size=8 alignment=8 stride=8 num_extra_inhabitants=2147483647 bitwise_takable=1))))))))
// CHECK-64:       (case name=otherA index=1)
// CHECK-64:       (case name=otherB index=2)
// CHECK-64:       (case name=otherC index=3)))
// CHECK-64:   (field name=a offset=32
// CHECK-64:     (single_payload_enum size=16 alignment=8 stride=16 num_extra_inhabitants=2147483644 bitwise_takable=1
// CHECK-64:       (case name=payload index=0 offset=0
// CHECK-64:         (struct size=16 alignment=8 stride=16 num_extra_inhabitants=2147483647 bitwise_takable=1
// CHECK-64:           (field name=_guts offset=0
// CHECK-64:             (struct size=16 alignment=8 stride=16 num_extra_inhabitants=2147483647 bitwise_takable=1
// CHECK-64:               (field name=_object offset=0
// CHECK-64:                 (struct size=16 alignment=8 stride=16 num_extra_inhabitants=2147483647 bitwise_takable=1
// CHECK-64:                   (field name=_countAndFlagsBits offset=0
// CHECK-64:                     (struct size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1
// CHECK-64:                       (field name=_value offset=0
// CHECK-64:                         (builtin size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1))))
// CHECK-64:                   (field name=_object offset=8
// CHECK-64:                     (builtin size=8 alignment=8 stride=8 num_extra_inhabitants=2147483647 bitwise_takable=1))))))))
// CHECK-64:       (case name=otherA index=1)
// CHECK-64:       (case name=otherB index=2)
// CHECK-64:       (case name=otherC index=3)))
// CHECK-64:   (field name=b offset=48
// CHECK-64:     (single_payload_enum size=16 alignment=8 stride=16 num_extra_inhabitants=2147483644 bitwise_takable=1
// CHECK-64:       (case name=payload index=0 offset=0
// CHECK-64:         (struct size=16 alignment=8 stride=16 num_extra_inhabitants=2147483647 bitwise_takable=1
// CHECK-64:           (field name=_guts offset=0
// CHECK-64:             (struct size=16 alignment=8 stride=16 num_extra_inhabitants=2147483647 bitwise_takable=1
// CHECK-64:               (field name=_object offset=0
// CHECK-64:                 (struct size=16 alignment=8 stride=16 num_extra_inhabitants=2147483647 bitwise_takable=1
// CHECK-64:                   (field name=_countAndFlagsBits offset=0
// CHECK-64:                     (struct size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1
// CHECK-64:                       (field name=_value offset=0
// CHECK-64:                         (builtin size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1))))
// CHECK-64:                   (field name=_object offset=8
// CHECK-64:                     (builtin size=8 alignment=8 stride=8 num_extra_inhabitants=2147483647 bitwise_takable=1))))))))
// CHECK-64:       (case name=otherA index=1)
// CHECK-64:       (case name=otherB index=2)
// CHECK-64:       (case name=otherC index=3)))
// CHECK-64:   (field name=c offset=64
// CHECK-64:     (single_payload_enum size=16 alignment=8 stride=16 num_extra_inhabitants=2147483644 bitwise_takable=1
// CHECK-64:       (case name=payload index=0 offset=0
// CHECK-64:         (struct size=16 alignment=8 stride=16 num_extra_inhabitants=2147483647 bitwise_takable=1
// CHECK-64:           (field name=_guts offset=0
// CHECK-64:             (struct size=16 alignment=8 stride=16 num_extra_inhabitants=2147483647 bitwise_takable=1
// CHECK-64:               (field name=_object offset=0
// CHECK-64:                 (struct size=16 alignment=8 stride=16 num_extra_inhabitants=2147483647 bitwise_takable=1
// CHECK-64:                   (field name=_countAndFlagsBits offset=0
// CHECK-64:                     (struct size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1
// CHECK-64:                       (field name=_value offset=0
// CHECK-64:                         (builtin size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1))))
// CHECK-64:                   (field name=_object offset=8
// CHECK-64:                     (builtin size=8 alignment=8 stride=8 num_extra_inhabitants=2147483647 bitwise_takable=1)))))))
// CHECK-64:       (case name=otherA index=1)
// CHECK-64:       (case name=otherB index=2)
// CHECK-64:       (case name=otherC index=3)))

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

// CHECK-64: Reflecting an object.
// CHECK-64: Instance pointer in child address space: 0x{{[0-9a-fA-F]+}}
// CHECK-64: Type reference:
// CHECK-64: (class reflect_enum_wip.ManyCasesManyPayloadsC)

// CHECK-64: Type info:
// CHECK-64: (class_instance size=81 alignment=8 stride=88 num_extra_inhabitants=0 bitwise_takable=1
// CHECK-64:   (field name=a offset=16
// CHECK-64:     (multi_payload_enum size=17 alignment=8 stride=24 num_extra_inhabitants=252 bitwise_takable=1
// CHECK-64:       (case name=a index=0 offset=0
// CHECK-64:         (struct size=16 alignment=8 stride=16 num_extra_inhabitants=2147483647 bitwise_takable=1
// CHECK-64:           (field name=_guts offset=0
// CHECK-64:             (struct size=16 alignment=8 stride=16 num_extra_inhabitants=2147483647 bitwise_takable=1
// CHECK-64:               (field name=_object offset=0
// CHECK-64:                 (struct size=16 alignment=8 stride=16 num_extra_inhabitants=2147483647 bitwise_takable=1
// CHECK-64:                   (field name=_countAndFlagsBits offset=0
// CHECK-64:                     (struct size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1
// CHECK-64:                       (field name=_value offset=0
// CHECK-64:                         (builtin size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1))))
// CHECK-64:                   (field name=_object offset=8
// CHECK-64:                     (builtin size=8 alignment=8 stride=8 num_extra_inhabitants=2147483647 bitwise_takable=1))))))))
// CHECK-64:       (case name=b index=1 offset=0
// CHECK-64:         (struct size=8 alignment=8 stride=8 num_extra_inhabitants=2147483647 bitwise_takable=1
// CHECK-64:           (field name=_buffer offset=0
// CHECK-64:             (struct size=8 alignment=8 stride=8 num_extra_inhabitants=2147483647 bitwise_takable=1
// CHECK-64:               (field name=_storage offset=0
// CHECK-64:                 (struct size=8 alignment=8 stride=8 num_extra_inhabitants=2147483647 bitwise_takable=1
// CHECK-64:                   (field name=rawValue offset=0
// CHECK-64:                     (builtin size=8 alignment=8 stride=8 num_extra_inhabitants=2147483647 bitwise_takable=1))))))))
// CHECK-64:       (case name=c index=2 offset=0
// CHECK-64:         (struct size=8 alignment=8 stride=8 num_extra_inhabitants=2147483647 bitwise_takable=1
// CHECK-64:           (field name=_variant offset=0
// CHECK-64:             (struct size=8 alignment=8 stride=8 num_extra_inhabitants=2147483647 bitwise_takable=1
// CHECK-64:               (field name=object offset=0
// CHECK-64:                 (struct size=8 alignment=8 stride=8 num_extra_inhabitants=2147483647 bitwise_takable=1
// CHECK-64:                   (field name=rawValue offset=0
// CHECK-64:                     (builtin size=8 alignment=8 stride=8 num_extra_inhabitants=2147483647 bitwise_takable=1))))))))
// CHECK-64:       (case name=extra index=3)))
// CHECK-64:   (field name=b offset=40
// CHECK-64:     (multi_payload_enum size=17 alignment=8 stride=24 num_extra_inhabitants=252 bitwise_takable=1
// CHECK-64:       (case name=a index=0 offset=0
// CHECK-64:         (struct size=16 alignment=8 stride=16 num_extra_inhabitants=2147483647 bitwise_takable=1
// CHECK-64:           (field name=_guts offset=0
// CHECK-64:             (struct size=16 alignment=8 stride=16 num_extra_inhabitants=2147483647 bitwise_takable=1
// CHECK-64:               (field name=_object offset=0
// CHECK-64:                 (struct size=16 alignment=8 stride=16 num_extra_inhabitants=2147483647 bitwise_takable=1
// CHECK-64:                   (field name=_countAndFlagsBits offset=0
// CHECK-64:                     (struct size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1
// CHECK-64:                       (field name=_value offset=0
// CHECK-64:                         (builtin size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1))))
// CHECK-64:                   (field name=_object offset=8
// CHECK-64:                     (builtin size=8 alignment=8 stride=8 num_extra_inhabitants=2147483647 bitwise_takable=1))))))))
// CHECK-64:       (case name=b index=1 offset=0
// CHECK-64:         (struct size=8 alignment=8 stride=8 num_extra_inhabitants=2147483647 bitwise_takable=1
// CHECK-64:           (field name=_buffer offset=0
// CHECK-64:             (struct size=8 alignment=8 stride=8 num_extra_inhabitants=2147483647 bitwise_takable=1
// CHECK-64:               (field name=_storage offset=0
// CHECK-64:                 (struct size=8 alignment=8 stride=8 num_extra_inhabitants=2147483647 bitwise_takable=1
// CHECK-64:                   (field name=rawValue offset=0
// CHECK-64:                     (builtin size=8 alignment=8 stride=8 num_extra_inhabitants=2147483647 bitwise_takable=1))))))))
// CHECK-64:       (case name=c index=2 offset=0
// CHECK-64:         (struct size=8 alignment=8 stride=8 num_extra_inhabitants=2147483647 bitwise_takable=1
// CHECK-64:           (field name=_variant offset=0
// CHECK-64:             (struct size=8 alignment=8 stride=8 num_extra_inhabitants=2147483647 bitwise_takable=1
// CHECK-64:               (field name=object offset=0
// CHECK-64:                 (struct size=8 alignment=8 stride=8 num_extra_inhabitants=2147483647 bitwise_takable=1
// CHECK-64:                   (field name=rawValue offset=0
// CHECK-64:                     (builtin size=8 alignment=8 stride=8 num_extra_inhabitants=2147483647 bitwise_takable=1))))))))
// CHECK-64:       (case name=extra index=3)))
// CHECK-64:   (field name=c offset=64
// CHECK-64:     (multi_payload_enum size=17 alignment=8 stride=24 num_extra_inhabitants=252 bitwise_takable=1
// CHECK-64:       (case name=a index=0 offset=0
// CHECK-64:         (struct size=16 alignment=8 stride=16 num_extra_inhabitants=2147483647 bitwise_takable=1
// CHECK-64:           (field name=_guts offset=0
// CHECK-64:             (struct size=16 alignment=8 stride=16 num_extra_inhabitants=2147483647 bitwise_takable=1
// CHECK-64:               (field name=_object offset=0
// CHECK-64:                 (struct size=16 alignment=8 stride=16 num_extra_inhabitants=2147483647 bitwise_takable=1
// CHECK-64:                   (field name=_countAndFlagsBits offset=0
// CHECK-64:                     (struct size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1
// CHECK-64:                       (field name=_value offset=0
// CHECK-64:                         (builtin size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1))))
// CHECK-64:                   (field name=_object offset=8
// CHECK-64:                     (builtin size=8 alignment=8 stride=8 num_extra_inhabitants=2147483647 bitwise_takable=1))))))))
// CHECK-64:       (case name=b index=1 offset=0
// CHECK-64:         (struct size=8 alignment=8 stride=8 num_extra_inhabitants=2147483647 bitwise_takable=1
// CHECK-64:           (field name=_buffer offset=0
// CHECK-64:             (struct size=8 alignment=8 stride=8 num_extra_inhabitants=2147483647 bitwise_takable=1
// CHECK-64:               (field name=_storage offset=0
// CHECK-64:                 (struct size=8 alignment=8 stride=8 num_extra_inhabitants=2147483647 bitwise_takable=1
// CHECK-64:                   (field name=rawValue offset=0
// CHECK-64:                     (builtin size=8 alignment=8 stride=8 num_extra_inhabitants=2147483647 bitwise_takable=1))))))))
// CHECK-64:       (case name=c index=2 offset=0
// CHECK-64:         (struct size=8 alignment=8 stride=8 num_extra_inhabitants=2147483647 bitwise_takable=1
// CHECK-64:           (field name=_variant offset=0
// CHECK-64:             (struct size=8 alignment=8 stride=8 num_extra_inhabitants=2147483647 bitwise_takable=1
// CHECK-64:               (field name=object offset=0
// CHECK-64:                 (struct size=8 alignment=8 stride=8 num_extra_inhabitants=2147483647 bitwise_takable=1
// CHECK-64:                   (field name=rawValue offset=0
// CHECK-64:                     (builtin size=8 alignment=8 stride=8 num_extra_inhabitants=2147483647 bitwise_takable=1))))))))
// CHECK-64:       (case name=extra index=3))))


reflect(enum: OneCaseNoPayload.only)

// CHECK-64: Reflecting an enum.
// CHECK-64: Instance pointer in child address space: 0x{{[0-9a-fA-F]+}}
// CHECK-64: Type reference:
// CHECK-64: (enum reflect_enum_wip.OneCaseNoPayload)

// CHECK-64: Type info:
// CHECK-64: (no_payload_enum size=0 alignment=1 stride=1 num_extra_inhabitants=0 bitwise_takable=1
// CHECK-64:   (case name=only index=0))

// CHECK-64: Enum value:
// CHECK-64: (enum_value name=only index=0)

reflect(enum: ManyCasesNoPayload.b)
// CHECK-64: Reflecting an enum.
// CHECK-64: Instance pointer in child address space: 0x{{[0-9a-fA-F]+}}
// CHECK-64: Type reference:
// CHECK-64: (enum reflect_enum_wip.ManyCasesNoPayload)

// CHECK-64: Type info:
// CHECK-64: (no_payload_enum size=1 alignment=1 stride=1 num_extra_inhabitants=252 bitwise_takable=1
// CHECK-64:   (case name=a index=0)
// CHECK-64:   (case name=b index=1)
// CHECK-64:   (case name=c index=2)
// CHECK-64:   (case name=d index=3))

// CHECK-64: Enum value:
// CHECK-64: (enum_value name=b index=1)

reflect(enum: VastNumberOfCasesNoPayload.option12)
// CHECK-64: Reflecting an enum.
// CHECK-64: Instance pointer in child address space: 0x{{[0-9a-fA-F]+}}
// CHECK-64: Type reference:
// CHECK-64: (enum reflect_enum_wip.VastNumberOfCasesNoPayload)

// CHECK-64: Type info:
// CHECK-64: (no_payload_enum size=2 alignment=2 stride=2 num_extra_inhabitants=65278 bitwise_takable=1
// CHECK-64:   (case name=option0 index=0)
// CHECK-64:   (case name=option1 index=1)
// CHECK-64:   (case name=option2 index=2)
// CHECK-64:   (case name=option3 index=3)
// CHECK-64:   (case name=option4 index=4)
// CHECK-64:   (case name=option5 index=5)
// CHECK-64:   (case name=option6 index=6)
// CHECK-64:   (case name=option7 index=7)
// CHECK-64:   (case name=option8 index=8)
// CHECK-64:   (case name=option9 index=9)
// CHECK-64:   (case name=option10 index=10)
// CHECK-64:   (case name=option11 index=11)
// CHECK-64:   (case name=option12 index=12)
// CHECK-64:   (case name=option13 index=13)
// CHECK-64:   (case name=option14 index=14)
// CHECK-64:   (case name=option15 index=15)
// CHECK-64:   (case name=option16 index=16)
// CHECK-64:   (case name=option17 index=17)
// CHECK-64:   (case name=option18 index=18)
// CHECK-64:   (case name=option19 index=19)
// CHECK-64:   (case name=option20 index=20)
// CHECK-64:   (case name=option21 index=21)
// CHECK-64:   (case name=option22 index=22)
// CHECK-64:   (case name=option23 index=23)
// CHECK-64:   (case name=option24 index=24)
// CHECK-64:   (case name=option25 index=25)
// CHECK-64:   (case name=option26 index=26)
// CHECK-64:   (case name=option27 index=27)
// CHECK-64:   (case name=option28 index=28)
// CHECK-64:   (case name=option29 index=29)
// CHECK-64:   (case name=option30 index=30)
// CHECK-64:   (case name=option31 index=31)
// CHECK-64:   (case name=option32 index=32)
// CHECK-64:   (case name=option33 index=33)
// CHECK-64:   (case name=option34 index=34)
// CHECK-64:   (case name=option35 index=35)
// CHECK-64:   (case name=option36 index=36)
// CHECK-64:   (case name=option37 index=37)
// CHECK-64:   (case name=option38 index=38)
// CHECK-64:   (case name=option39 index=39)
// CHECK-64:   (case name=option40 index=40)
// CHECK-64:   (case name=option41 index=41)
// CHECK-64:   (case name=option42 index=42)
// CHECK-64:   (case name=option43 index=43)
// CHECK-64:   (case name=option44 index=44)
// CHECK-64:   (case name=option45 index=45)
// CHECK-64:   (case name=option46 index=46)
// CHECK-64:   (case name=option47 index=47)
// CHECK-64:   (case name=option48 index=48)
// CHECK-64:   (case name=option49 index=49)
// CHECK-64:   (case name=option50 index=50)
// CHECK-64:   (case name=option51 index=51)
// CHECK-64:   (case name=option52 index=52)
// CHECK-64:   (case name=option53 index=53)
// CHECK-64:   (case name=option54 index=54)
// CHECK-64:   (case name=option55 index=55)
// CHECK-64:   (case name=option56 index=56)
// CHECK-64:   (case name=option57 index=57)
// CHECK-64:   (case name=option58 index=58)
// CHECK-64:   (case name=option59 index=59)
// CHECK-64:   (case name=option60 index=60)
// CHECK-64:   (case name=option61 index=61)
// CHECK-64:   (case name=option62 index=62)
// CHECK-64:   (case name=option63 index=63)
// CHECK-64:   (case name=option64 index=64)
// CHECK-64:   (case name=option65 index=65)
// CHECK-64:   (case name=option66 index=66)
// CHECK-64:   (case name=option67 index=67)
// CHECK-64:   (case name=option68 index=68)
// CHECK-64:   (case name=option69 index=69)
// CHECK-64:   (case name=option70 index=70)
// CHECK-64:   (case name=option71 index=71)
// CHECK-64:   (case name=option72 index=72)
// CHECK-64:   (case name=option73 index=73)
// CHECK-64:   (case name=option74 index=74)
// CHECK-64:   (case name=option75 index=75)
// CHECK-64:   (case name=option76 index=76)
// CHECK-64:   (case name=option77 index=77)
// CHECK-64:   (case name=option78 index=78)
// CHECK-64:   (case name=option79 index=79)
// CHECK-64:   (case name=option80 index=80)
// CHECK-64:   (case name=option81 index=81)
// CHECK-64:   (case name=option82 index=82)
// CHECK-64:   (case name=option83 index=83)
// CHECK-64:   (case name=option84 index=84)
// CHECK-64:   (case name=option85 index=85)
// CHECK-64:   (case name=option86 index=86)
// CHECK-64:   (case name=option87 index=87)
// CHECK-64:   (case name=option88 index=88)
// CHECK-64:   (case name=option89 index=89)
// CHECK-64:   (case name=option90 index=90)
// CHECK-64:   (case name=option91 index=91)
// CHECK-64:   (case name=option92 index=92)
// CHECK-64:   (case name=option93 index=93)
// CHECK-64:   (case name=option94 index=94)
// CHECK-64:   (case name=option95 index=95)
// CHECK-64:   (case name=option96 index=96)
// CHECK-64:   (case name=option97 index=97)
// CHECK-64:   (case name=option98 index=98)
// CHECK-64:   (case name=option99 index=99)
// CHECK-64:   (case name=option100 index=100)
// CHECK-64:   (case name=option101 index=101)
// CHECK-64:   (case name=option102 index=102)
// CHECK-64:   (case name=option103 index=103)
// CHECK-64:   (case name=option104 index=104)
// CHECK-64:   (case name=option105 index=105)
// CHECK-64:   (case name=option106 index=106)
// CHECK-64:   (case name=option107 index=107)
// CHECK-64:   (case name=option108 index=108)
// CHECK-64:   (case name=option109 index=109)
// CHECK-64:   (case name=option110 index=110)
// CHECK-64:   (case name=option111 index=111)
// CHECK-64:   (case name=option112 index=112)
// CHECK-64:   (case name=option113 index=113)
// CHECK-64:   (case name=option114 index=114)
// CHECK-64:   (case name=option115 index=115)
// CHECK-64:   (case name=option116 index=116)
// CHECK-64:   (case name=option117 index=117)
// CHECK-64:   (case name=option118 index=118)
// CHECK-64:   (case name=option119 index=119)
// CHECK-64:   (case name=option120 index=120)
// CHECK-64:   (case name=option121 index=121)
// CHECK-64:   (case name=option122 index=122)
// CHECK-64:   (case name=option123 index=123)
// CHECK-64:   (case name=option124 index=124)
// CHECK-64:   (case name=option125 index=125)
// CHECK-64:   (case name=option126 index=126)
// CHECK-64:   (case name=option127 index=127)
// CHECK-64:   (case name=option128 index=128)
// CHECK-64:   (case name=option129 index=129)
// CHECK-64:   (case name=option130 index=130)
// CHECK-64:   (case name=option131 index=131)
// CHECK-64:   (case name=option132 index=132)
// CHECK-64:   (case name=option133 index=133)
// CHECK-64:   (case name=option134 index=134)
// CHECK-64:   (case name=option135 index=135)
// CHECK-64:   (case name=option136 index=136)
// CHECK-64:   (case name=option137 index=137)
// CHECK-64:   (case name=option138 index=138)
// CHECK-64:   (case name=option139 index=139)
// CHECK-64:   (case name=option140 index=140)
// CHECK-64:   (case name=option141 index=141)
// CHECK-64:   (case name=option142 index=142)
// CHECK-64:   (case name=option143 index=143)
// CHECK-64:   (case name=option144 index=144)
// CHECK-64:   (case name=option145 index=145)
// CHECK-64:   (case name=option146 index=146)
// CHECK-64:   (case name=option147 index=147)
// CHECK-64:   (case name=option148 index=148)
// CHECK-64:   (case name=option149 index=149)
// CHECK-64:   (case name=option150 index=150)
// CHECK-64:   (case name=option151 index=151)
// CHECK-64:   (case name=option152 index=152)
// CHECK-64:   (case name=option153 index=153)
// CHECK-64:   (case name=option154 index=154)
// CHECK-64:   (case name=option155 index=155)
// CHECK-64:   (case name=option156 index=156)
// CHECK-64:   (case name=option157 index=157)
// CHECK-64:   (case name=option158 index=158)
// CHECK-64:   (case name=option159 index=159)
// CHECK-64:   (case name=option160 index=160)
// CHECK-64:   (case name=option161 index=161)
// CHECK-64:   (case name=option162 index=162)
// CHECK-64:   (case name=option163 index=163)
// CHECK-64:   (case name=option164 index=164)
// CHECK-64:   (case name=option165 index=165)
// CHECK-64:   (case name=option166 index=166)
// CHECK-64:   (case name=option167 index=167)
// CHECK-64:   (case name=option168 index=168)
// CHECK-64:   (case name=option169 index=169)
// CHECK-64:   (case name=option170 index=170)
// CHECK-64:   (case name=option171 index=171)
// CHECK-64:   (case name=option172 index=172)
// CHECK-64:   (case name=option173 index=173)
// CHECK-64:   (case name=option174 index=174)
// CHECK-64:   (case name=option175 index=175)
// CHECK-64:   (case name=option176 index=176)
// CHECK-64:   (case name=option177 index=177)
// CHECK-64:   (case name=option178 index=178)
// CHECK-64:   (case name=option179 index=179)
// CHECK-64:   (case name=option180 index=180)
// CHECK-64:   (case name=option181 index=181)
// CHECK-64:   (case name=option182 index=182)
// CHECK-64:   (case name=option183 index=183)
// CHECK-64:   (case name=option184 index=184)
// CHECK-64:   (case name=option185 index=185)
// CHECK-64:   (case name=option186 index=186)
// CHECK-64:   (case name=option187 index=187)
// CHECK-64:   (case name=option188 index=188)
// CHECK-64:   (case name=option189 index=189)
// CHECK-64:   (case name=option190 index=190)
// CHECK-64:   (case name=option191 index=191)
// CHECK-64:   (case name=option192 index=192)
// CHECK-64:   (case name=option193 index=193)
// CHECK-64:   (case name=option194 index=194)
// CHECK-64:   (case name=option195 index=195)
// CHECK-64:   (case name=option196 index=196)
// CHECK-64:   (case name=option197 index=197)
// CHECK-64:   (case name=option198 index=198)
// CHECK-64:   (case name=option199 index=199)
// CHECK-64:   (case name=option200 index=200)
// CHECK-64:   (case name=option201 index=201)
// CHECK-64:   (case name=option202 index=202)
// CHECK-64:   (case name=option203 index=203)
// CHECK-64:   (case name=option204 index=204)
// CHECK-64:   (case name=option205 index=205)
// CHECK-64:   (case name=option206 index=206)
// CHECK-64:   (case name=option207 index=207)
// CHECK-64:   (case name=option208 index=208)
// CHECK-64:   (case name=option209 index=209)
// CHECK-64:   (case name=option210 index=210)
// CHECK-64:   (case name=option211 index=211)
// CHECK-64:   (case name=option212 index=212)
// CHECK-64:   (case name=option213 index=213)
// CHECK-64:   (case name=option214 index=214)
// CHECK-64:   (case name=option215 index=215)
// CHECK-64:   (case name=option216 index=216)
// CHECK-64:   (case name=option217 index=217)
// CHECK-64:   (case name=option218 index=218)
// CHECK-64:   (case name=option219 index=219)
// CHECK-64:   (case name=option220 index=220)
// CHECK-64:   (case name=option221 index=221)
// CHECK-64:   (case name=option222 index=222)
// CHECK-64:   (case name=option223 index=223)
// CHECK-64:   (case name=option224 index=224)
// CHECK-64:   (case name=option225 index=225)
// CHECK-64:   (case name=option226 index=226)
// CHECK-64:   (case name=option227 index=227)
// CHECK-64:   (case name=option228 index=228)
// CHECK-64:   (case name=option229 index=229)
// CHECK-64:   (case name=option230 index=230)
// CHECK-64:   (case name=option231 index=231)
// CHECK-64:   (case name=option232 index=232)
// CHECK-64:   (case name=option233 index=233)
// CHECK-64:   (case name=option234 index=234)
// CHECK-64:   (case name=option235 index=235)
// CHECK-64:   (case name=option236 index=236)
// CHECK-64:   (case name=option237 index=237)
// CHECK-64:   (case name=option238 index=238)
// CHECK-64:   (case name=option239 index=239)
// CHECK-64:   (case name=option240 index=240)
// CHECK-64:   (case name=option241 index=241)
// CHECK-64:   (case name=option242 index=242)
// CHECK-64:   (case name=option243 index=243)
// CHECK-64:   (case name=option244 index=244)
// CHECK-64:   (case name=option245 index=245)
// CHECK-64:   (case name=option246 index=246)
// CHECK-64:   (case name=option247 index=247)
// CHECK-64:   (case name=option248 index=248)
// CHECK-64:   (case name=option249 index=249)
// CHECK-64:   (case name=option250 index=250)
// CHECK-64:   (case name=option251 index=251)
// CHECK-64:   (case name=option252 index=252)
// CHECK-64:   (case name=option253 index=253)
// CHECK-64:   (case name=option254 index=254)
// CHECK-64:   (case name=option255 index=255)
// CHECK-64:   (case name=option256 index=256)
// CHECK-64:   (case name=option257 index=257))

// CHECK-64: Enum value:
// CHECK-64: (enum_value name=option12 index=12)

reflect(enum: VastNumberOfCasesNoPayload.option256)

// CHECK-64: Reflecting an enum.
// CHECK-64: Instance pointer in child address space: 0x{{[0-9a-fA-F]+}}
// CHECK-64: Type reference:
// CHECK-64: (enum reflect_enum_wip.VastNumberOfCasesNoPayload)

// CHECK-64: Type info:
// CHECK-64: (no_payload_enum size=2 alignment=2 stride=2 num_extra_inhabitants=65278 bitwise_takable=1
// CHECK-64:   (case name=option0 index=0)
// CHECK-64:   (case name=option1 index=1)
// CHECK-64:   (case name=option2 index=2)
// CHECK-64:   (case name=option3 index=3)
// CHECK-64:   (case name=option4 index=4)
// CHECK-64:   (case name=option5 index=5)
// CHECK-64:   (case name=option6 index=6)
// CHECK-64:   (case name=option7 index=7)
// CHECK-64:   (case name=option8 index=8)
// CHECK-64:   (case name=option9 index=9)
// CHECK-64:   (case name=option10 index=10)
// CHECK-64:   (case name=option11 index=11)
// CHECK-64:   (case name=option12 index=12)
// CHECK-64:   (case name=option13 index=13)
// CHECK-64:   (case name=option14 index=14)
// CHECK-64:   (case name=option15 index=15)
// CHECK-64:   (case name=option16 index=16)
// CHECK-64:   (case name=option17 index=17)
// CHECK-64:   (case name=option18 index=18)
// CHECK-64:   (case name=option19 index=19)
// CHECK-64:   (case name=option20 index=20)
// CHECK-64:   (case name=option21 index=21)
// CHECK-64:   (case name=option22 index=22)
// CHECK-64:   (case name=option23 index=23)
// CHECK-64:   (case name=option24 index=24)
// CHECK-64:   (case name=option25 index=25)
// CHECK-64:   (case name=option26 index=26)
// CHECK-64:   (case name=option27 index=27)
// CHECK-64:   (case name=option28 index=28)
// CHECK-64:   (case name=option29 index=29)
// CHECK-64:   (case name=option30 index=30)
// CHECK-64:   (case name=option31 index=31)
// CHECK-64:   (case name=option32 index=32)
// CHECK-64:   (case name=option33 index=33)
// CHECK-64:   (case name=option34 index=34)
// CHECK-64:   (case name=option35 index=35)
// CHECK-64:   (case name=option36 index=36)
// CHECK-64:   (case name=option37 index=37)
// CHECK-64:   (case name=option38 index=38)
// CHECK-64:   (case name=option39 index=39)
// CHECK-64:   (case name=option40 index=40)
// CHECK-64:   (case name=option41 index=41)
// CHECK-64:   (case name=option42 index=42)
// CHECK-64:   (case name=option43 index=43)
// CHECK-64:   (case name=option44 index=44)
// CHECK-64:   (case name=option45 index=45)
// CHECK-64:   (case name=option46 index=46)
// CHECK-64:   (case name=option47 index=47)
// CHECK-64:   (case name=option48 index=48)
// CHECK-64:   (case name=option49 index=49)
// CHECK-64:   (case name=option50 index=50)
// CHECK-64:   (case name=option51 index=51)
// CHECK-64:   (case name=option52 index=52)
// CHECK-64:   (case name=option53 index=53)
// CHECK-64:   (case name=option54 index=54)
// CHECK-64:   (case name=option55 index=55)
// CHECK-64:   (case name=option56 index=56)
// CHECK-64:   (case name=option57 index=57)
// CHECK-64:   (case name=option58 index=58)
// CHECK-64:   (case name=option59 index=59)
// CHECK-64:   (case name=option60 index=60)
// CHECK-64:   (case name=option61 index=61)
// CHECK-64:   (case name=option62 index=62)
// CHECK-64:   (case name=option63 index=63)
// CHECK-64:   (case name=option64 index=64)
// CHECK-64:   (case name=option65 index=65)
// CHECK-64:   (case name=option66 index=66)
// CHECK-64:   (case name=option67 index=67)
// CHECK-64:   (case name=option68 index=68)
// CHECK-64:   (case name=option69 index=69)
// CHECK-64:   (case name=option70 index=70)
// CHECK-64:   (case name=option71 index=71)
// CHECK-64:   (case name=option72 index=72)
// CHECK-64:   (case name=option73 index=73)
// CHECK-64:   (case name=option74 index=74)
// CHECK-64:   (case name=option75 index=75)
// CHECK-64:   (case name=option76 index=76)
// CHECK-64:   (case name=option77 index=77)
// CHECK-64:   (case name=option78 index=78)
// CHECK-64:   (case name=option79 index=79)
// CHECK-64:   (case name=option80 index=80)
// CHECK-64:   (case name=option81 index=81)
// CHECK-64:   (case name=option82 index=82)
// CHECK-64:   (case name=option83 index=83)
// CHECK-64:   (case name=option84 index=84)
// CHECK-64:   (case name=option85 index=85)
// CHECK-64:   (case name=option86 index=86)
// CHECK-64:   (case name=option87 index=87)
// CHECK-64:   (case name=option88 index=88)
// CHECK-64:   (case name=option89 index=89)
// CHECK-64:   (case name=option90 index=90)
// CHECK-64:   (case name=option91 index=91)
// CHECK-64:   (case name=option92 index=92)
// CHECK-64:   (case name=option93 index=93)
// CHECK-64:   (case name=option94 index=94)
// CHECK-64:   (case name=option95 index=95)
// CHECK-64:   (case name=option96 index=96)
// CHECK-64:   (case name=option97 index=97)
// CHECK-64:   (case name=option98 index=98)
// CHECK-64:   (case name=option99 index=99)
// CHECK-64:   (case name=option100 index=100)
// CHECK-64:   (case name=option101 index=101)
// CHECK-64:   (case name=option102 index=102)
// CHECK-64:   (case name=option103 index=103)
// CHECK-64:   (case name=option104 index=104)
// CHECK-64:   (case name=option105 index=105)
// CHECK-64:   (case name=option106 index=106)
// CHECK-64:   (case name=option107 index=107)
// CHECK-64:   (case name=option108 index=108)
// CHECK-64:   (case name=option109 index=109)
// CHECK-64:   (case name=option110 index=110)
// CHECK-64:   (case name=option111 index=111)
// CHECK-64:   (case name=option112 index=112)
// CHECK-64:   (case name=option113 index=113)
// CHECK-64:   (case name=option114 index=114)
// CHECK-64:   (case name=option115 index=115)
// CHECK-64:   (case name=option116 index=116)
// CHECK-64:   (case name=option117 index=117)
// CHECK-64:   (case name=option118 index=118)
// CHECK-64:   (case name=option119 index=119)
// CHECK-64:   (case name=option120 index=120)
// CHECK-64:   (case name=option121 index=121)
// CHECK-64:   (case name=option122 index=122)
// CHECK-64:   (case name=option123 index=123)
// CHECK-64:   (case name=option124 index=124)
// CHECK-64:   (case name=option125 index=125)
// CHECK-64:   (case name=option126 index=126)
// CHECK-64:   (case name=option127 index=127)
// CHECK-64:   (case name=option128 index=128)
// CHECK-64:   (case name=option129 index=129)
// CHECK-64:   (case name=option130 index=130)
// CHECK-64:   (case name=option131 index=131)
// CHECK-64:   (case name=option132 index=132)
// CHECK-64:   (case name=option133 index=133)
// CHECK-64:   (case name=option134 index=134)
// CHECK-64:   (case name=option135 index=135)
// CHECK-64:   (case name=option136 index=136)
// CHECK-64:   (case name=option137 index=137)
// CHECK-64:   (case name=option138 index=138)
// CHECK-64:   (case name=option139 index=139)
// CHECK-64:   (case name=option140 index=140)
// CHECK-64:   (case name=option141 index=141)
// CHECK-64:   (case name=option142 index=142)
// CHECK-64:   (case name=option143 index=143)
// CHECK-64:   (case name=option144 index=144)
// CHECK-64:   (case name=option145 index=145)
// CHECK-64:   (case name=option146 index=146)
// CHECK-64:   (case name=option147 index=147)
// CHECK-64:   (case name=option148 index=148)
// CHECK-64:   (case name=option149 index=149)
// CHECK-64:   (case name=option150 index=150)
// CHECK-64:   (case name=option151 index=151)
// CHECK-64:   (case name=option152 index=152)
// CHECK-64:   (case name=option153 index=153)
// CHECK-64:   (case name=option154 index=154)
// CHECK-64:   (case name=option155 index=155)
// CHECK-64:   (case name=option156 index=156)
// CHECK-64:   (case name=option157 index=157)
// CHECK-64:   (case name=option158 index=158)
// CHECK-64:   (case name=option159 index=159)
// CHECK-64:   (case name=option160 index=160)
// CHECK-64:   (case name=option161 index=161)
// CHECK-64:   (case name=option162 index=162)
// CHECK-64:   (case name=option163 index=163)
// CHECK-64:   (case name=option164 index=164)
// CHECK-64:   (case name=option165 index=165)
// CHECK-64:   (case name=option166 index=166)
// CHECK-64:   (case name=option167 index=167)
// CHECK-64:   (case name=option168 index=168)
// CHECK-64:   (case name=option169 index=169)
// CHECK-64:   (case name=option170 index=170)
// CHECK-64:   (case name=option171 index=171)
// CHECK-64:   (case name=option172 index=172)
// CHECK-64:   (case name=option173 index=173)
// CHECK-64:   (case name=option174 index=174)
// CHECK-64:   (case name=option175 index=175)
// CHECK-64:   (case name=option176 index=176)
// CHECK-64:   (case name=option177 index=177)
// CHECK-64:   (case name=option178 index=178)
// CHECK-64:   (case name=option179 index=179)
// CHECK-64:   (case name=option180 index=180)
// CHECK-64:   (case name=option181 index=181)
// CHECK-64:   (case name=option182 index=182)
// CHECK-64:   (case name=option183 index=183)
// CHECK-64:   (case name=option184 index=184)
// CHECK-64:   (case name=option185 index=185)
// CHECK-64:   (case name=option186 index=186)
// CHECK-64:   (case name=option187 index=187)
// CHECK-64:   (case name=option188 index=188)
// CHECK-64:   (case name=option189 index=189)
// CHECK-64:   (case name=option190 index=190)
// CHECK-64:   (case name=option191 index=191)
// CHECK-64:   (case name=option192 index=192)
// CHECK-64:   (case name=option193 index=193)
// CHECK-64:   (case name=option194 index=194)
// CHECK-64:   (case name=option195 index=195)
// CHECK-64:   (case name=option196 index=196)
// CHECK-64:   (case name=option197 index=197)
// CHECK-64:   (case name=option198 index=198)
// CHECK-64:   (case name=option199 index=199)
// CHECK-64:   (case name=option200 index=200)
// CHECK-64:   (case name=option201 index=201)
// CHECK-64:   (case name=option202 index=202)
// CHECK-64:   (case name=option203 index=203)
// CHECK-64:   (case name=option204 index=204)
// CHECK-64:   (case name=option205 index=205)
// CHECK-64:   (case name=option206 index=206)
// CHECK-64:   (case name=option207 index=207)
// CHECK-64:   (case name=option208 index=208)
// CHECK-64:   (case name=option209 index=209)
// CHECK-64:   (case name=option210 index=210)
// CHECK-64:   (case name=option211 index=211)
// CHECK-64:   (case name=option212 index=212)
// CHECK-64:   (case name=option213 index=213)
// CHECK-64:   (case name=option214 index=214)
// CHECK-64:   (case name=option215 index=215)
// CHECK-64:   (case name=option216 index=216)
// CHECK-64:   (case name=option217 index=217)
// CHECK-64:   (case name=option218 index=218)
// CHECK-64:   (case name=option219 index=219)
// CHECK-64:   (case name=option220 index=220)
// CHECK-64:   (case name=option221 index=221)
// CHECK-64:   (case name=option222 index=222)
// CHECK-64:   (case name=option223 index=223)
// CHECK-64:   (case name=option224 index=224)
// CHECK-64:   (case name=option225 index=225)
// CHECK-64:   (case name=option226 index=226)
// CHECK-64:   (case name=option227 index=227)
// CHECK-64:   (case name=option228 index=228)
// CHECK-64:   (case name=option229 index=229)
// CHECK-64:   (case name=option230 index=230)
// CHECK-64:   (case name=option231 index=231)
// CHECK-64:   (case name=option232 index=232)
// CHECK-64:   (case name=option233 index=233)
// CHECK-64:   (case name=option234 index=234)
// CHECK-64:   (case name=option235 index=235)
// CHECK-64:   (case name=option236 index=236)
// CHECK-64:   (case name=option237 index=237)
// CHECK-64:   (case name=option238 index=238)
// CHECK-64:   (case name=option239 index=239)
// CHECK-64:   (case name=option240 index=240)
// CHECK-64:   (case name=option241 index=241)
// CHECK-64:   (case name=option242 index=242)
// CHECK-64:   (case name=option243 index=243)
// CHECK-64:   (case name=option244 index=244)
// CHECK-64:   (case name=option245 index=245)
// CHECK-64:   (case name=option246 index=246)
// CHECK-64:   (case name=option247 index=247)
// CHECK-64:   (case name=option248 index=248)
// CHECK-64:   (case name=option249 index=249)
// CHECK-64:   (case name=option250 index=250)
// CHECK-64:   (case name=option251 index=251)
// CHECK-64:   (case name=option252 index=252)
// CHECK-64:   (case name=option253 index=253)
// CHECK-64:   (case name=option254 index=254)
// CHECK-64:   (case name=option255 index=255)
// CHECK-64:   (case name=option256 index=256)
// CHECK-64:   (case name=option257 index=257))

// CHECK-64: Enum value:
// CHECK-64: (enum_value name=option256 index=256)

reflect(enum: ManyCasesOneIntPayload.payload(77))

// CHECK-64: Reflecting an enum.
// CHECK-64: Instance pointer in child address space: 0x{{[0-9a-fA-F]+}}
// CHECK-64: Type reference:
// CHECK-64: (enum reflect_enum_wip.ManyCasesOneIntPayload)

// CHECK-64: Type info:
// CHECK-64: (single_payload_enum size=9 alignment=8 stride=16 num_extra_inhabitants=0 bitwise_takable=1
// CHECK-64:   (case name=payload index=0 offset=0
// CHECK-64:     (struct size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1
// CHECK-64:       (field name=_value offset=0
// CHECK-64:         (builtin size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1))))
// CHECK-64:   (case name=otherA index=1)
// CHECK-64:   (case name=otherB index=2)
// CHECK-64:   (case name=otherC index=3))

// CHECK-64: Enum value:
// CHECK-64: (enum_value name=payload index=0
// CHECK-64:     (struct size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1
// CHECK-64:       (field name=_value offset=0
// CHECK-64:         (builtin size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1)))
// CHECK-64: )


reflect(enum: ManyCasesOneStringPayload.payload("hello, world"))

// CHECK-64: Reflecting an enum.
// CHECK-64: Instance pointer in child address space: 0x{{[0-9a-fA-F]+}}
// CHECK-64: Type reference:
// CHECK-64: (enum reflect_enum_wip.ManyCasesOneStringPayload)

// CHECK-64: Type info:
// CHECK-64: (single_payload_enum size=16 alignment=8 stride=16 num_extra_inhabitants=2147483644 bitwise_takable=1
// CHECK-64:   (case name=payload index=0 offset=0
// CHECK-64:     (struct size=16 alignment=8 stride=16 num_extra_inhabitants=2147483647 bitwise_takable=1
// CHECK-64:       (field name=_guts offset=0
// CHECK-64:         (struct size=16 alignment=8 stride=16 num_extra_inhabitants=2147483647 bitwise_takable=1
// CHECK-64:           (field name=_object offset=0
// CHECK-64:             (struct size=16 alignment=8 stride=16 num_extra_inhabitants=2147483647 bitwise_takable=1
// CHECK-64:               (field name=_countAndFlagsBits offset=0
// CHECK-64:                 (struct size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1
// CHECK-64:                   (field name=_value offset=0
// CHECK-64:                     (builtin size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1))))
// CHECK-64:               (field name=_object offset=8
// CHECK-64:                 (builtin size=8 alignment=8 stride=8 num_extra_inhabitants=2147483647 bitwise_takable=1))))))))
// CHECK-64:   (case name=otherA index=1)
// CHECK-64:   (case name=otherB index=2)
// CHECK-64:   (case name=otherC index=3))

// CHECK-64: Enum value:
// CHECK-64: (enum_value name=payload index=0
// CHECK-64: (struct size=16 alignment=8 stride=16 num_extra_inhabitants=2147483647 bitwise_takable=1
// CHECK-64:   (field name=_guts offset=0
// CHECK-64:     (struct size=16 alignment=8 stride=16 num_extra_inhabitants=2147483647 bitwise_takable=1
// CHECK-64:       (field name=_object offset=0
// CHECK-64:         (struct size=16 alignment=8 stride=16 num_extra_inhabitants=2147483647 bitwise_takable=1
// CHECK-64:           (field name=_countAndFlagsBits offset=0
// CHECK-64:             (struct size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1
// CHECK-64:               (field name=_value offset=0
// CHECK-64:                 (builtin size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1))))
// CHECK-64:           (field name=_object offset=8
// CHECK-64:             (builtin size=8 alignment=8 stride=8 num_extra_inhabitants=2147483647 bitwise_takable=1)))))))
// CHECK-64: )

reflect(enum: ManyCasesOneStringPayload.otherB)

// CHECK-64: Reflecting an enum.
// CHECK-64: Instance pointer in child address space: 0x{{[0-9a-fA-F]+}}
// CHECK-64: Type reference:
// CHECK-64: (enum reflect_enum_wip.ManyCasesOneStringPayload)

// CHECK-64: Type info:
// CHECK-64: (single_payload_enum size=16 alignment=8 stride=16 num_extra_inhabitants=2147483644 bitwise_takable=1
// CHECK-64:   (case name=payload index=0 offset=0
// CHECK-64:     (struct size=16 alignment=8 stride=16 num_extra_inhabitants=2147483647 bitwise_takable=1
// CHECK-64:       (field name=_guts offset=0
// CHECK-64:         (struct size=16 alignment=8 stride=16 num_extra_inhabitants=2147483647 bitwise_takable=1
// CHECK-64:           (field name=_object offset=0
// CHECK-64:             (struct size=16 alignment=8 stride=16 num_extra_inhabitants=2147483647 bitwise_takable=1
// CHECK-64:               (field name=_countAndFlagsBits offset=0
// CHECK-64:                 (struct size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1
// CHECK-64:                   (field name=_value offset=0
// CHECK-64:                     (builtin size=8 alignment=8 stride=8 num_extra_inhabitants=0 bitwise_takable=1))))
// CHECK-64:               (field name=_object offset=8
// CHECK-64:                 (builtin size=8 alignment=8 stride=8 num_extra_inhabitants=2147483647 bitwise_takable=1))))))))
// CHECK-64:   (case name=otherA index=1)
// CHECK-64:   (case name=otherB index=2)
// CHECK-64:   (case name=otherC index=3))

// CHECK-64: Enum value:
// CHECK-64: (enum_value name=otherB index=2)


//reflect(enum: ManyCasesManyPayloads.a("hi, world"))

doneReflecting()

// CHECK-64: Done.

// CHECK-32: Done.
