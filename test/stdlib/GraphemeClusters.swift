//
// Test that complex emojis are correctly segmented into Grapheme Clusters
// Checks for problems with flag emojis, family emojis using zero width joiners
// and skin tone modifiers in stdlb/public/core/StringCharacterView.swift.
//
// http://getemoji.com/
//

// RUN: %target-run-simple-swift
// REQUIRES: executable_test

import StdlibUnittest

var str="👦🏾👧🏾👨🏾👩🏾👴🏾👵🏾👶🏾👱🏾👮🏾👲🏾👳🏾👷🏾👸🏾💂🏾🎅🏾👼🏾💆🏾💇🏾👰🏾🙍🏾🙎🏾🙅🏾🙆🏾💁🏾🙋🏾🙇🏾🙌🏾🙏🏾🚶🏾🏃🏾💃🏾💪🏾👈🏾👉🏾☝️🏾👆🏾🖕🏾👇🏾✌️🏾🖖🏾🤘🏾🖐🏾✊🏾✋🏾👊🏾👌🏾👍🏾👎🏾👋🏾👏🏾👐🏾✍🏾💅🏾👂🏾👃🏾🚣🏾🛀🏾🏄🏾🏇🏾🏊🏾⛹🏾🏋🏾🚴🏾🚵🏾👨‍👨‍👧‍👧👨‍👨‍👧‍👧👨‍👨‍👧‍👧kjh🇭🇺🇭🇺🇭🇺kgdf👨‍👨‍👧‍👧😦😧😢😥😪😓😭😵😲🤐😷🤒🤕😴💤💩😈👿👹👺💀👻👽🤖😺😸😹😻😼😽🙀😿😾🙌👏👋👍👊✊✌️👌✋💪🙏☝️👆👇👈👉🖕🤘🖖✍️💅👄👅👂👃👁👀👤🗣👶👦👧👨👩👱👴👵👲👳👮👷💂🕵🎅👼👸👰🚶🏃💃👯👫👬👭🙇💁🙅🙆🙋🙎🙍💇💆💑👩‍❤️‍👩👨‍❤️‍👨💏👩‍❤️‍💋‍👩👨‍❤️‍💋‍👨👪👨‍👩‍👧👨‍👩‍👧‍👦👨‍👩‍👦‍👦👨‍👩‍👧‍👧👩‍👩‍👦👩‍👩‍👧👩‍👩‍👧‍👦👩‍👩‍👦‍👦👩‍👩‍👧‍👧👨‍👨‍👦👨‍👨‍👧👨‍👨‍👧‍👦👨‍👨‍👦‍👦👨‍👨‍👧‍👧👫👬👭👚👕👖👔👗👙👘💄💋👣👠👡👢👞👟👒🎩⛑🎓👑🎒👝👛👜💼👓🕶💍🌂🇭🇺🇫🇷🇭🇺🇫🇷🇭🇺❤️💛💙💜💔❣️💕💞💓💗💖💘💝💟☮✝️☪🕉☸✡️🔯🕎☯️☦🛐⛎♈️♉️♊️♋️♌️♍️♎️♏️♐️♑️♒️♓️🆔⚛🈳🈹☢☣📴📳🈶🈚️🈸🈺🈷️✴️🆚🉑💮🉐㊙️㊗️🈴🈵🈲🅰️🅱️🆎🆑🅾️🆘⛔️📛🚫❌⭕️💢♨️🚷🚯🚳🚱🔞📵❗️❕❓❔‼️⁉️💯🔅🔆🔱⚜〽️⚠️🚸🔰♻️🈯️💹❇️✳️❎✅💠🌀➿🌐Ⓜ️🏧🈂️🛂🛃🛄🛅♿️🚭🚾🅿️🚰🚹🚺🚼🚻🚮🎦📶🈁🆖🆗🆙🆒🆕🆓0️⃣1️⃣2️⃣3️⃣4️⃣5️⃣6️⃣7️⃣8️⃣9️⃣🔟🔢▶️⏸⏯⏹⏺⏭⏮⏩⏪🔀🔁🔂◀️🔼🔽⏫⏬➡️⬅️⬆️⬇️↗️↘️↙️↖️↕️↔️🔄↪️↩️⤴️⤵️#️⃣*️⃣ℹ️🔤🔡🔠🔣🎵🎶〰️➰✔️🔃➕➖➗✖️💲💱©️®️™️🔚🔙🔛🔝🔜☑️🔘⚪️⚫️🔴🔵🔸🔹🔶🔷🔺▪️▫️⬛️⬜️🔻◼️◻️◾️◽️🔲🔳🔈🔉🔊🔇📣📢🔔🔕🃏🀄️♠️♣️♥️♦️🎴👁‍🗨💭🗯💬🕐🕑🕒🕓🕔🕕🕖🕗🕘🕙🕚🕛🕜🕝🕞🕟🕠🕡🕢🕣🕤🕥"

var EmojiTests = TestSuite("GraphemeClusters")

EmojiTests.test("segmentation") {
    var forward = Array<String>(), backward = Array<String>()

    var start = str.startIndex
    while start < str.endIndex{
        let next = str.index(start, offsetBy:1)
    //    print(str[start..<next])
        forward.append(str[start..<next])
        start = next
    }

    var end = str.endIndex
    while end > str.startIndex{
        let next = str.index(end, offsetBy:-1)
    //    print(str[next..<end])
        backward.append(str[next..<end])
        end = next
    }

    expectEqual(500, forward.count)
    expectEqual(forward, backward.reversed())
}

runAllTests()
