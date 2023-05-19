#define NUM_REGS 30

// Apply `macro` to "all" registers. Skip x18 since it's reserved, and x30 since
// it's the link register.
#define ALL_REGS(macro)                                                        \
  macro( 0)                                                                    \
  macro( 1)                                                                    \
  macro( 2)                                                                    \
  macro( 3)                                                                    \
  macro( 4)                                                                    \
  macro( 5)                                                                    \
  macro( 6)                                                                    \
  macro( 7)                                                                    \
  macro( 8)                                                                    \
  macro( 9)                                                                    \
  macro(10)                                                                    \
  macro(11)                                                                    \
  macro(12)                                                                    \
  macro(13)                                                                    \
  macro(14)                                                                    \
  macro(15)                                                                    \
  macro(16)                                                                    \
  macro(17)                                                                    \
  macro(19)                                                                    \
  macro(20)                                                                    \
  macro(21)                                                                    \
  macro(22)                                                                    \
  macro(23)                                                                    \
  macro(24)                                                                    \
  macro(25)                                                                    \
  macro(26)                                                                    \
  macro(27)                                                                    \
  macro(28)

// Apply `macro` with the given parameters to all registers that have
// specialized entrypoints. That's the same as ALL_REGS, minus x0 (the standard
// entrypoint covers that), x16/x17 (temporary registers used as linker glue),
// and x29 (the link register).
#define FUNCTION_REGS(macro, ...)                                              \
  macro( 1, __VA_ARGS__)                                                       \
  macro( 2, __VA_ARGS__)                                                       \
  macro( 3, __VA_ARGS__)                                                       \
  macro( 4, __VA_ARGS__)                                                       \
  macro( 5, __VA_ARGS__)                                                       \
  macro( 6, __VA_ARGS__)                                                       \
  macro( 7, __VA_ARGS__)                                                       \
  macro( 8, __VA_ARGS__)                                                       \
  macro( 9, __VA_ARGS__)                                                       \
  macro(10, __VA_ARGS__)                                                       \
  macro(11, __VA_ARGS__)                                                       \
  macro(12, __VA_ARGS__)                                                       \
  macro(13, __VA_ARGS__)                                                       \
  macro(14, __VA_ARGS__)                                                       \
  macro(15, __VA_ARGS__)                                                       \
  macro(19, __VA_ARGS__)                                                       \
  macro(20, __VA_ARGS__)                                                       \
  macro(21, __VA_ARGS__)                                                       \
  macro(22, __VA_ARGS__)                                                       \
  macro(23, __VA_ARGS__)                                                       \
  macro(24, __VA_ARGS__)                                                       \
  macro(25, __VA_ARGS__)                                                       \
  macro(26, __VA_ARGS__)                                                       \
  macro(27, __VA_ARGS__)                                                       \
  macro(28, __VA_ARGS__)

// Apply `macro` to each function that gets specialized entrypoints. Also pass
// 1 if the function is a retain variant, and 0 if it's a release variant.
#define ALL_FUNCTIONS(macro)                                                   \
  macro(swift_retain, 1)                                                       \
  macro(swift_release, 0)                                                      \
  macro(swift_bridgeObjectRetain, 1)                                           \
  macro(swift_bridgeObjectRelease, 0)

// Emit declarations for variables called xN stored in xN, initialized with
// regs[N].
#define PASS_REGS_HELPER(num) \
  register void *x ## num asm ("x" #num) = regs[num];
#define PASS_REGS ALL_REGS(PASS_REGS_HELPER)

// Emit an entry in an asm inputs list containing "r" (xN).
#define REG_INPUTS_HELPER(num) \
  "r" (x ## num),
#define REG_INPUTS ALL_REGS(REG_INPUTS_HELPER)

// Make a function called call_function_xN that calls function_xN with registers
// set to the contents of the given registers array.
#define MAKE_CALL_FUNC(reg, func)                                              \
  static inline void call_##func##_x##reg(void **regs) {                       \
    PASS_REGS                                                                  \
    asm("bl _" #func "_x" #reg : : REG_INPUTS "i"(0));                         \
  }

// Make a call_function_xN for each specialized function and register.
#define MAKE_ALL_CALL_FUNCS(function, isRetain)                                \
  FUNCTION_REGS(MAKE_CALL_FUNC, function)
ALL_FUNCTIONS(MAKE_ALL_CALL_FUNCS)

// Call `call` with each call_function_xN function created above, as well as the
// base function name, the register it operates on, and whether it's a retain
// function.
static inline void foreachRRFunction(void (*call)(void (*)(void **regs),
                                                  const char *name, int reg,
                                                  int isRetain)) {
#define CALL_ONE_FUNCTION(reg, function, isRetain)                             \
  call(call_##function##_x##reg, #function, reg, isRetain);
#define CALL_WITH_FUNCTIONS(function, isRetain)                                \
  FUNCTION_REGS(CALL_ONE_FUNCTION, function, isRetain)

  ALL_FUNCTIONS(CALL_WITH_FUNCTIONS)
}
