:orphan:

Calling Convention Summary
==========================

Below is a summary of the calling conventions used on macOS and iOS.

The `ABI stability manifesto <../ABIStabilityManifesto.md>`_ gives more details
on the use of the Swift error return and ``self`` registers, while `The Swift
Calling Convention <CallingConvention.rst>`_ covers the specifics in more
details.  (The Swift ``self`` register is known in other documents as the
"Context register".)

x86-64
------

See `Apple x86-64 Documentation`_, `System V ABI AMD64 Processor Supplement`_.

.. _Apple x86-64 Documentation: https://developer.apple.com/library/archive/documentation/DeveloperTools/Conceptual/LowLevelABI/140-x86-64_Function_Calling_Conventions/x86_64.html
.. _System V ABI AMD64 Processor Supplement: https://www.uclibc.org/docs/psABI-x86_64.pdf

Register usage
^^^^^^^^^^^^^^

+-----------+----------------------------------+----------+----------+----------+
| Register  | Purpose                          | C++      | ObjC     | Swift    |
+===========+==================================+==========+==========+==========+
| ``rax``   | Return value; also, for varargs, |          |          |          |
|           | number of ``xmm`` registers used |          |          |          |
+-----------+----------------------------------+----------+----------+----------+
| ``rbx``   | Callee-saved register            |          |          |          |
+-----------+----------------------------------+----------+----------+----------+
| ``rdi``   | Integer argument 1               | ``this`` | ``self`` |          |
+-----------+----------------------------------+----------+----------+----------+
| ``rsi``   | Integer argument 2               |          | ``_cmd`` |          |
+-----------+----------------------------------+----------+----------+----------+
| ``rdx``   | Integer argument 3               |          |          |          |
|           | (2nd return value)               |          |          |          |
+-----------+----------------------------------+----------+----------+----------+
| ``rcx``   | Integer argument 4               |          |          |          |
|           | (3rd return value)               |          |          |          |
+-----------+----------------------------------+----------+----------+----------+
| ``r8``    | Integer argument 5               |          |          |          |
|           | (4th return value)               |          |          |          |
+-----------+----------------------------------+----------+----------+----------+
| ``r9``    | Integer argument 6               |          |          |          |
+-----------+----------------------------------+----------+----------+----------+
| ``r12``   | Callee-saved register            |          |          | Error    |
|           |                                  |          |          | return   |
+-----------+----------------------------------+----------+----------+----------+
| ``r13``   | Callee-saved register            |          |          | ``self`` |
+-----------+----------------------------------+----------+----------+----------+
| ``r14``   | Callee-saved register            |          |          | Async    |
|           |                                  |          |          | context  |
+-----------+----------------------------------+----------+----------+----------+
| ``r15``   | Callee-saved register            |          |          |          |
|           | (other platforms use as GOT ptr) |          |          |          |
+-----------+----------------------------------+----------+----------+----------+
| ``st0``   | Used to return ``long double``   |          |          |          |
|           | values                           |          |          |          |
+-----------+----------------------------------+----------+----------+----------+
| ``st1``   | Used to return ``long double``   |          |          |          |
|           | values                           |          |          |          |
+-----------+----------------------------------+----------+----------+----------+
| ``xmm0``- | Floating point arguments 1-8     |          |          |          |
| ``xmm7``  | (``xmm0``-``xmm3`` also used     |          |          |          |
|           | for return)                      |          |          |          |
+-----------+----------------------------------+----------+----------+----------+
| ``rsp``   | Stack pointer                    |          |          |          |
+-----------+----------------------------------+----------+----------+----------+
| ``rbp``   | Callee-saved register,           |          |          |          |
|           | used as frame pointer            |          |          |          |
+-----------+----------------------------------+----------+----------+----------+

Stack frame
^^^^^^^^^^^

On function entry, ``rsp+8`` is **16-byte aligned**, i.e. the start of the memory
arguments is 16-byte aligned; the initial stack pointer is shown below as "entry
``rsp``",  but a typical non-leaf function will start by doing::

  push %rbp
  mov  %rsp, %rbp
  sub  <local-size>, %rsp

Frameless leaf functions, however, will often not set up the frame pointer,
``rbp``, in which case they may refer to arguments relative to ``rsp`` instead.

+---------------+---------------+------------------------+
|               | ``rbp+8n+16`` | memory argument *n*    |
|               |               |                        |
|               | ...           | ...                    |
|               |               |                        |
|               | ``rbp+16``    | memory argument 0      |
+---------------+-----------+---+------------------------+
| ↓ Current Frame           |           ↑ Previous Frame |
+---------------+-----------+---+------------------------+
|               | ``rbp+8``     | return address         |
|               |               |                        |
+---------------+---------------+------------------------+
| entry ``rsp`` | ``rbp``       | previous ``rbp`` value |
+---------------+---------------+------------------------+
|               | ``rbp-8``     |                        |
|               |               |                        |
|               | ...           |      local storage     |
|               |               |                        |
|               | ``rsp``       |                        |
+---------------+---------------+------------------------+
|               | ``rsp-8``     |                        |
|               |               |                        |
|               | ...           |        red zone        |
|               |               |                        |
|               | ``rsp-128``   |                        |
+---------------+---------------+------------------------+


ARM64
-----

See `Apple ARM64 Documentation`_, `Procedure Call Standard for the Arm 64-bit Architecture`_.

.. _Apple ARM64 Documentation: https://developer.apple.com/documentation/xcode/writing-arm64-code-for-apple-platforms
.. _Procedure Call Standard for the Arm 64-bit Architecture: https://github.com/ARM-software/abi-aa/blob/main/aapcs64/aapcs64.rst

Register usage
^^^^^^^^^^^^^^

+----------+---------+-------------------------+----------+----------+----------+
| Register | Special | Purpose                 | C++      | ObjC     | Swift    |
+==========+=========+=========================+==========+==========+==========+
| ``x0``   |         | Integer argument 1      | ``this`` | ``self`` |          |
|          |         | (1st return value)      |          |          |          |
+----------+---------+-------------------------+----------+----------+----------+
| ``x1``   |         | Integer argument 2      |          | ``_cmd`` |          |
|          |         | (2nd return value)      |          |          |          |
+----------+---------+-------------------------+----------+----------+----------+
| ``x2``   |         | Integer argument 3      |          |          |          |
|          |         | (3rd return value)      |          |          |          |
+----------+---------+-------------------------+----------+----------+----------+
| ``x3``   |         | Integer argument 4      |          |          |          |
|          |         | (4th return value)      |          |          |          |
+----------+---------+-------------------------+----------+----------+----------+
| ``x4``-  |         | Integer arguments 5-8   |          |          |          |
| ``x7``   |         |                         |          |          |          |
+----------+---------+-------------------------+----------+----------+----------+
| ``x8``   |         | Indirect result         |          |          |          |
|          |         | location register       |          |          |          |
+----------+---------+-------------------------+----------+----------+----------+
| ``x16``, | ``ip0``,| Scratch registers (used |          |          |          |
| ``x17``  | ``ip1`` | by dyld, can be used    |          |          |          |
|          |         | freely otherwise)       |          |          |          |
+----------+---------+-------------------------+----------+----------+----------+
| ``x18``  |         | RESERVED **DO NOT USE** |          |          |          |
+----------+---------+-------------------------+----------+----------+----------+
| ``x19``  |         | Callee-saved register   |          |          |          |
+----------+---------+-------------------------+----------+----------+----------+
| ``x20``  |         | Callee-saved register   |          |          | ``self`` |
+----------+---------+-------------------------+----------+----------+----------+
| ``x21``  |         | Callee-saved register   |          |          | Error    |
|          |         |                         |          |          | return   |
+----------+---------+-------------------------+----------+----------+----------+
| ``x22``  |         | Callee-saved register   |          |          | Async    |
|          |         |                         |          |          | context  |
+----------+---------+-------------------------+----------+----------+----------+
| ``x23``- |         | Callee-saved registers  |          |          |          |
| ``x28``  |         |                         |          |          |          |
+----------+---------+-------------------------+----------+----------+----------+
| ``x29``  | ``fp``  | Frame pointer           |          |          |          |
+----------+---------+-------------------------+----------+----------+----------+
| ``x30``  | ``lr``  | Link register           |          |          |          |
+----------+---------+-------------------------+----------+----------+----------+
| ``sp``   |         | Stack pointer           |          |          |          |
+----------+---------+-------------------------+----------+----------+----------+
| ``v0``-  |         | Floating point/SIMD     |          |          |          |
| ``v7``   |         | arguments 1-8           |          |          |          |
|          |         | (also for return)       |          |          |          |
+----------+---------+-------------------------+----------+----------+----------+
| ``v8``-  |         | Callee-saved registers  |          |          |          |
| ``v15``  |         | (**lower 64-bits only**)|          |          |          |
+----------+---------+-------------------------+----------+----------+----------+

Stack frame
^^^^^^^^^^^

The stack pointer is **16-byte aligned**; on function entry, ``sp`` points at
the location shown by "entry ``sp``" below.  As with x86, frameless leaf
functions may not set up ``fp``, in which case they will use ``sp`` relative
accesses.

+--------------+---------------+------------------------+
|              | ``fp+8n+16``  | last memory argument   |
|              |               |                        |
|              | ...           | ...                    |
|              |               |                        |
|              | ``fp+16``     | memory argument 0 [1]_ |
+--------------+------------+--+------------------------+
| ↓ Current Frame           |          ↑ Previous Frame |
+--------------+------------+--+------------------------+
| entry ``sp`` | ``fp+8``      | saved ``lr``           |
|              |               | (return address)       |
+--------------+---------------+------------------------+
|              | ``fp``        | previous ``fp`` value  |
+--------------+---------------+------------------------+
|              | ``fp-8``      |                        |
|              |               |                        |
|              | ...           |      local storage     |
|              |               |                        |
|              | ``sp``        |                        |
+--------------+---------------+------------------------+
|              | ``sp-8``      |                        |
|              |               |                        |
|              | ...           |        red zone        |
|              |               |                        |
|              | ``sp-128``    |                        |
+--------------+---------------+------------------------+

.. [1] See Apple documentation, however.  Unlike the official ARM64 ABI, we pack
       arguments, so this might also hold argument 1, argument 2 and so on.
