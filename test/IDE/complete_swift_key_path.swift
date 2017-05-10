// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=PART_1 | %FileCheck %s -check-prefix=PERSON-MEMBER
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=PART_2 | %FileCheck %s -check-prefix=PERSON-MEMBER
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=PART_3 | %FileCheck %s -check-prefix=PERSON-MEMBER
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=PART_4 | %FileCheck %s -check-prefix=PERSON-MEMBER-OPT
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=PART_5 | %FileCheck %s -check-prefix=PERSON-MEMBER
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=PART_6 | %FileCheck %s -check-prefix=PERSON-MEMBER

class Person {
    var name: String
    var friends: [Person] = []
    var bestFriend: Person? = nil
    init(name: String) {
        self.name = name
    }
    func getName() -> String { return name }
}

let keyPath1 = \Person.#^PART_1^#
let keyPath2 = \Person.friends[0].#^PART_2^#
let keyPath3 = \Person.friends[0].friends[0].friends[0].#^PART_3^#
let keyPath4 = \Person.bestFriend?.#^PART_4^#
let keyPath5 = \Person.friends.[0].friends[0].friends[0].#^PART_5^#
let keyPath6 = \Person.friends.[0].friends.[0].friends.[0].#^PART_6^#

// PERSON-MEMBER: Begin completions, 3 items
// PERSON-MEMBER-NEXT: Decl[InstanceVar]/CurrNominal:      name[#String#]; name=name
// PERSON-MEMBER-NEXT: Decl[InstanceVar]/CurrNominal:      friends[#[Person]#]; name=friends
// PERSON-MEMBER-NEXT: Decl[InstanceVar]/CurrNominal:      bestFriend[#Person?#]; name=bestFriend 

// PERSON-MEMBER-OPT: Begin completions, 6 items
// PERSON-MEMBER-OPT-NEXT: Decl[InstanceVar]/CurrNominal/Erase[1]: ?.name[#String#]; name=name
// PERSON-MEMBER-OPT-NEXT: Decl[InstanceVar]/CurrNominal/Erase[1]: ?.friends[#[Person]#]; name=friends
// PERSON-MEMBER-OPT-NEXT: Decl[InstanceVar]/CurrNominal/Erase[1]: ?.bestFriend[#Person?#]; name=bestFriend
