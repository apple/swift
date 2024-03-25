// RUN: %batch-code-completion

struct Mixer<each Source: Signal>: Signal {}

print(Mixer(#^COMPLETE^#))

// COMPLETE: Decl[Constructor]/CurrNominal/Flair[ArgLabels]: ['('][')'][#Mixer<repeat each Source>#];