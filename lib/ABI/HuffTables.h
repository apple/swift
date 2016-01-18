#ifndef SWIFT_MANGLER_HUFFMAN_H
#define SWIFT_MANGLER_HUFFMAN_H
#include <assert.h>
#include <utility>
#include "llvm/ADT/APInt.h"
using APInt = llvm::APInt;
// This file is autogenerated. Do not modify this file.
// Processing text files: CBC_Compressed.txt.cbc
namespace Huffman {
// The charset that the fragment indices can use:
const unsigned CharsetLength = 64;
const unsigned LongestEncodingLength = 8;
const char *Charset = "0123456789_abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ$";
const int IndexOfChar[] =  { -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,63,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,0,1,2,3,4,5,6,7,8,9,-1,-1,-1,-1,-1,-1,-1,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,-1,-1,-1,-1,10,-1,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1 };
std::pair<char, unsigned> variable_decode(uint64_t tailbits) {
if ((tailbits & 1) == 0) {
 tailbits/=2;
 if ((tailbits & 1) == 0) {
  tailbits/=2;
  if ((tailbits & 1) == 0) {
   tailbits/=2;
   if ((tailbits & 1) == 0) {
    tailbits/=2;
    if ((tailbits & 1) == 0) {
     tailbits/=2;
     return {'S', 5};
    }
    if ((tailbits & 1) == 1) {
     tailbits/=2;
     if ((tailbits & 1) == 0) {
      tailbits/=2;
      if ((tailbits & 1) == 0) {
       tailbits/=2;
       return {'z', 7};
      }
      if ((tailbits & 1) == 1) {
       tailbits/=2;
       return {'G', 7};
      }
     }
     if ((tailbits & 1) == 1) {
      tailbits/=2;
      if ((tailbits & 1) == 0) {
       tailbits/=2;
       return {'L', 7};
      }
      if ((tailbits & 1) == 1) {
       tailbits/=2;
       return {'M', 7};
      }
     }
    }
   }
   if ((tailbits & 1) == 1) {
    tailbits/=2;
    if ((tailbits & 1) == 0) {
     tailbits/=2;
     if ((tailbits & 1) == 0) {
      tailbits/=2;
      return {'r', 6};
     }
     if ((tailbits & 1) == 1) {
      tailbits/=2;
      return {'b', 6};
     }
    }
    if ((tailbits & 1) == 1) {
     tailbits/=2;
     if ((tailbits & 1) == 0) {
      tailbits/=2;
      if ((tailbits & 1) == 0) {
       tailbits/=2;
       return {'D', 7};
      }
      if ((tailbits & 1) == 1) {
       tailbits/=2;
       return {'I', 7};
      }
     }
     if ((tailbits & 1) == 1) {
      tailbits/=2;
      return {'l', 6};
     }
    }
   }
  }
  if ((tailbits & 1) == 1) {
   tailbits/=2;
   if ((tailbits & 1) == 0) {
    tailbits/=2;
    if ((tailbits & 1) == 0) {
     tailbits/=2;
     if ((tailbits & 1) == 0) {
      tailbits/=2;
      return {'d', 6};
     }
     if ((tailbits & 1) == 1) {
      tailbits/=2;
      return {'o', 6};
     }
    }
    if ((tailbits & 1) == 1) {
     tailbits/=2;
     if ((tailbits & 1) == 0) {
      tailbits/=2;
      return {'c', 6};
     }
     if ((tailbits & 1) == 1) {
      tailbits/=2;
      return {'9', 6};
     }
    }
   }
   if ((tailbits & 1) == 1) {
    tailbits/=2;
    return {'1', 4};
   }
  }
 }
 if ((tailbits & 1) == 1) {
  tailbits/=2;
  if ((tailbits & 1) == 0) {
   tailbits/=2;
   if ((tailbits & 1) == 0) {
    tailbits/=2;
    if ((tailbits & 1) == 0) {
     tailbits/=2;
     return {'3', 5};
    }
    if ((tailbits & 1) == 1) {
     tailbits/=2;
     if ((tailbits & 1) == 0) {
      tailbits/=2;
      if ((tailbits & 1) == 0) {
       tailbits/=2;
       return {'P', 7};
      }
      if ((tailbits & 1) == 1) {
       tailbits/=2;
       return {'y', 7};
      }
     }
     if ((tailbits & 1) == 1) {
      tailbits/=2;
      return {'n', 6};
     }
    }
   }
   if ((tailbits & 1) == 1) {
    tailbits/=2;
    if ((tailbits & 1) == 0) {
     tailbits/=2;
     if ((tailbits & 1) == 0) {
      tailbits/=2;
      if ((tailbits & 1) == 0) {
       tailbits/=2;
       return {'q', 7};
      }
      if ((tailbits & 1) == 1) {
       tailbits/=2;
       return {'0', 7};
      }
     }
     if ((tailbits & 1) == 1) {
      tailbits/=2;
      return {'f', 6};
     }
    }
    if ((tailbits & 1) == 1) {
     tailbits/=2;
     if ((tailbits & 1) == 0) {
      tailbits/=2;
      if ((tailbits & 1) == 0) {
       tailbits/=2;
       return {'V', 7};
      }
      if ((tailbits & 1) == 1) {
       tailbits/=2;
       return {'A', 7};
      }
     }
     if ((tailbits & 1) == 1) {
      tailbits/=2;
      if ((tailbits & 1) == 0) {
       tailbits/=2;
       if ((tailbits & 1) == 0) {
        tailbits/=2;
        return {'$', 8};
       }
       if ((tailbits & 1) == 1) {
        tailbits/=2;
        return {'X', 8};
       }
      }
      if ((tailbits & 1) == 1) {
       tailbits/=2;
       return {'j', 7};
      }
     }
    }
   }
  }
  if ((tailbits & 1) == 1) {
   tailbits/=2;
   if ((tailbits & 1) == 0) {
    tailbits/=2;
    if ((tailbits & 1) == 0) {
     tailbits/=2;
     return {'e', 5};
    }
    if ((tailbits & 1) == 1) {
     tailbits/=2;
     if ((tailbits & 1) == 0) {
      tailbits/=2;
      return {'7', 6};
     }
     if ((tailbits & 1) == 1) {
      tailbits/=2;
      if ((tailbits & 1) == 0) {
       tailbits/=2;
       return {'k', 7};
      }
      if ((tailbits & 1) == 1) {
       tailbits/=2;
       return {'w', 7};
      }
     }
    }
   }
   if ((tailbits & 1) == 1) {
    tailbits/=2;
    if ((tailbits & 1) == 0) {
     tailbits/=2;
     return {'T', 5};
    }
    if ((tailbits & 1) == 1) {
     tailbits/=2;
     if ((tailbits & 1) == 0) {
      tailbits/=2;
      return {'8', 6};
     }
     if ((tailbits & 1) == 1) {
      tailbits/=2;
      if ((tailbits & 1) == 0) {
       tailbits/=2;
       return {'B', 7};
      }
      if ((tailbits & 1) == 1) {
       tailbits/=2;
       return {'N', 7};
      }
     }
    }
   }
  }
 }
}
if ((tailbits & 1) == 1) {
 tailbits/=2;
 if ((tailbits & 1) == 0) {
  tailbits/=2;
  if ((tailbits & 1) == 0) {
   tailbits/=2;
   if ((tailbits & 1) == 0) {
    tailbits/=2;
    if ((tailbits & 1) == 0) {
     tailbits/=2;
     if ((tailbits & 1) == 0) {
      tailbits/=2;
      return {'s', 6};
     }
     if ((tailbits & 1) == 1) {
      tailbits/=2;
      return {'6', 6};
     }
    }
    if ((tailbits & 1) == 1) {
     tailbits/=2;
     if ((tailbits & 1) == 0) {
      tailbits/=2;
      if ((tailbits & 1) == 0) {
       tailbits/=2;
       if ((tailbits & 1) == 0) {
        tailbits/=2;
        return {'Q', 8};
       }
       if ((tailbits & 1) == 1) {
        tailbits/=2;
        return {'U', 8};
       }
      }
      if ((tailbits & 1) == 1) {
       tailbits/=2;
       return {'C', 7};
      }
     }
     if ((tailbits & 1) == 1) {
      tailbits/=2;
      if ((tailbits & 1) == 0) {
       tailbits/=2;
       return {'h', 7};
      }
      if ((tailbits & 1) == 1) {
       tailbits/=2;
       if ((tailbits & 1) == 0) {
        tailbits/=2;
        return {'Z', 8};
       }
       if ((tailbits & 1) == 1) {
        tailbits/=2;
        return {'K', 8};
       }
      }
     }
    }
   }
   if ((tailbits & 1) == 1) {
    tailbits/=2;
    if ((tailbits & 1) == 0) {
     tailbits/=2;
     return {'2', 5};
    }
    if ((tailbits & 1) == 1) {
     tailbits/=2;
     if ((tailbits & 1) == 0) {
      tailbits/=2;
      return {'t', 6};
     }
     if ((tailbits & 1) == 1) {
      tailbits/=2;
      if ((tailbits & 1) == 0) {
       tailbits/=2;
       return {'p', 7};
      }
      if ((tailbits & 1) == 1) {
       tailbits/=2;
       if ((tailbits & 1) == 0) {
        tailbits/=2;
        return {'R', 8};
       }
       if ((tailbits & 1) == 1) {
        tailbits/=2;
        return {'H', 8};
       }
      }
     }
    }
   }
  }
  if ((tailbits & 1) == 1) {
   tailbits/=2;
   if ((tailbits & 1) == 0) {
    tailbits/=2;
    if ((tailbits & 1) == 0) {
     tailbits/=2;
     if ((tailbits & 1) == 0) {
      tailbits/=2;
      return {'5', 6};
     }
     if ((tailbits & 1) == 1) {
      tailbits/=2;
      if ((tailbits & 1) == 0) {
       tailbits/=2;
       return {'x', 7};
      }
      if ((tailbits & 1) == 1) {
       tailbits/=2;
       return {'m', 7};
      }
     }
    }
    if ((tailbits & 1) == 1) {
     tailbits/=2;
     if ((tailbits & 1) == 0) {
      tailbits/=2;
      return {'i', 6};
     }
     if ((tailbits & 1) == 1) {
      tailbits/=2;
      if ((tailbits & 1) == 0) {
       tailbits/=2;
       return {'F', 7};
      }
      if ((tailbits & 1) == 1) {
       tailbits/=2;
       return {'g', 7};
      }
     }
    }
   }
   if ((tailbits & 1) == 1) {
    tailbits/=2;
    return {'_', 4};
   }
  }
 }
 if ((tailbits & 1) == 1) {
  tailbits/=2;
  if ((tailbits & 1) == 0) {
   tailbits/=2;
   if ((tailbits & 1) == 0) {
    tailbits/=2;
    if ((tailbits & 1) == 0) {
     tailbits/=2;
     if ((tailbits & 1) == 0) {
      tailbits/=2;
      if ((tailbits & 1) == 0) {
       tailbits/=2;
       if ((tailbits & 1) == 0) {
        tailbits/=2;
        return {'W', 8};
       }
       if ((tailbits & 1) == 1) {
        tailbits/=2;
        return {'O', 8};
       }
      }
      if ((tailbits & 1) == 1) {
       tailbits/=2;
       return {'u', 7};
      }
     }
     if ((tailbits & 1) == 1) {
      tailbits/=2;
      return {'a', 6};
     }
    }
    if ((tailbits & 1) == 1) {
     tailbits/=2;
     if ((tailbits & 1) == 0) {
      tailbits/=2;
      if ((tailbits & 1) == 0) {
       tailbits/=2;
       return {'v', 7};
      }
      if ((tailbits & 1) == 1) {
       tailbits/=2;
       return {'E', 7};
      }
     }
     if ((tailbits & 1) == 1) {
      tailbits/=2;
      return {'4', 6};
     }
    }
   }
   if ((tailbits & 1) == 1) {
    tailbits/=2;
    return {'Y', 4};
   }
  }
  if ((tailbits & 1) == 1) {
   tailbits/=2;
   return {'J', 3};
  }
 }
} 
 assert(false); return {0, 0};
}
void variable_encode(uint64_t &bits, uint64_t &num_bits, char ch) {
if (ch == 'S') {/*00000*/ bits = 0; num_bits = 5; return; }
if (ch == 'z') {/*0010000*/ bits = 16; num_bits = 7; return; }
if (ch == 'G') {/*1010000*/ bits = 80; num_bits = 7; return; }
if (ch == 'L') {/*0110000*/ bits = 48; num_bits = 7; return; }
if (ch == 'M') {/*1110000*/ bits = 112; num_bits = 7; return; }
if (ch == 'r') {/*001000*/ bits = 8; num_bits = 6; return; }
if (ch == 'b') {/*101000*/ bits = 40; num_bits = 6; return; }
if (ch == 'D') {/*0011000*/ bits = 24; num_bits = 7; return; }
if (ch == 'I') {/*1011000*/ bits = 88; num_bits = 7; return; }
if (ch == 'l') {/*111000*/ bits = 56; num_bits = 6; return; }
if (ch == 'd') {/*000100*/ bits = 4; num_bits = 6; return; }
if (ch == 'o') {/*100100*/ bits = 36; num_bits = 6; return; }
if (ch == 'c') {/*010100*/ bits = 20; num_bits = 6; return; }
if (ch == '9') {/*110100*/ bits = 52; num_bits = 6; return; }
if (ch == '1') {/*1100*/ bits = 12; num_bits = 4; return; }
if (ch == '3') {/*00010*/ bits = 2; num_bits = 5; return; }
if (ch == 'P') {/*0010010*/ bits = 18; num_bits = 7; return; }
if (ch == 'y') {/*1010010*/ bits = 82; num_bits = 7; return; }
if (ch == 'n') {/*110010*/ bits = 50; num_bits = 6; return; }
if (ch == 'q') {/*0001010*/ bits = 10; num_bits = 7; return; }
if (ch == '0') {/*1001010*/ bits = 74; num_bits = 7; return; }
if (ch == 'f') {/*101010*/ bits = 42; num_bits = 6; return; }
if (ch == 'V') {/*0011010*/ bits = 26; num_bits = 7; return; }
if (ch == 'A') {/*1011010*/ bits = 90; num_bits = 7; return; }
if (ch == '$') {/*00111010*/ bits = 58; num_bits = 8; return; }
if (ch == 'X') {/*10111010*/ bits = 186; num_bits = 8; return; }
if (ch == 'j') {/*1111010*/ bits = 122; num_bits = 7; return; }
if (ch == 'e') {/*00110*/ bits = 6; num_bits = 5; return; }
if (ch == '7') {/*010110*/ bits = 22; num_bits = 6; return; }
if (ch == 'k') {/*0110110*/ bits = 54; num_bits = 7; return; }
if (ch == 'w') {/*1110110*/ bits = 118; num_bits = 7; return; }
if (ch == 'T') {/*01110*/ bits = 14; num_bits = 5; return; }
if (ch == '8') {/*011110*/ bits = 30; num_bits = 6; return; }
if (ch == 'B') {/*0111110*/ bits = 62; num_bits = 7; return; }
if (ch == 'N') {/*1111110*/ bits = 126; num_bits = 7; return; }
if (ch == 's') {/*000001*/ bits = 1; num_bits = 6; return; }
if (ch == '6') {/*100001*/ bits = 33; num_bits = 6; return; }
if (ch == 'Q') {/*00010001*/ bits = 17; num_bits = 8; return; }
if (ch == 'U') {/*10010001*/ bits = 145; num_bits = 8; return; }
if (ch == 'C') {/*1010001*/ bits = 81; num_bits = 7; return; }
if (ch == 'h') {/*0110001*/ bits = 49; num_bits = 7; return; }
if (ch == 'Z') {/*01110001*/ bits = 113; num_bits = 8; return; }
if (ch == 'K') {/*11110001*/ bits = 241; num_bits = 8; return; }
if (ch == '2') {/*01001*/ bits = 9; num_bits = 5; return; }
if (ch == 't') {/*011001*/ bits = 25; num_bits = 6; return; }
if (ch == 'p') {/*0111001*/ bits = 57; num_bits = 7; return; }
if (ch == 'R') {/*01111001*/ bits = 121; num_bits = 8; return; }
if (ch == 'H') {/*11111001*/ bits = 249; num_bits = 8; return; }
if (ch == '5') {/*000101*/ bits = 5; num_bits = 6; return; }
if (ch == 'x') {/*0100101*/ bits = 37; num_bits = 7; return; }
if (ch == 'm') {/*1100101*/ bits = 101; num_bits = 7; return; }
if (ch == 'i') {/*010101*/ bits = 21; num_bits = 6; return; }
if (ch == 'F') {/*0110101*/ bits = 53; num_bits = 7; return; }
if (ch == 'g') {/*1110101*/ bits = 117; num_bits = 7; return; }
if (ch == '_') {/*1101*/ bits = 13; num_bits = 4; return; }
if (ch == 'W') {/*00000011*/ bits = 3; num_bits = 8; return; }
if (ch == 'O') {/*10000011*/ bits = 131; num_bits = 8; return; }
if (ch == 'u') {/*1000011*/ bits = 67; num_bits = 7; return; }
if (ch == 'a') {/*100011*/ bits = 35; num_bits = 6; return; }
if (ch == 'v') {/*0010011*/ bits = 19; num_bits = 7; return; }
if (ch == 'E') {/*1010011*/ bits = 83; num_bits = 7; return; }
if (ch == '4') {/*110011*/ bits = 51; num_bits = 6; return; }
if (ch == 'Y') {/*1011*/ bits = 11; num_bits = 4; return; }
if (ch == 'J') {/*111*/ bits = 7; num_bits = 3; return; }
assert(false);
}
} // namespace
#endif /* SWIFT_MANGLER_HUFFMAN_H */
