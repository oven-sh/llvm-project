## Check that llvm-bolt folds the Cortex-A53 erratum 843419 patch veneers that
## `ld.lld --fix-cortex-a53-843419` creates back into the patched function.
##
## The linker moves the load that follows a page-crossing ADRP into a veneer
## (__CortexA53843419_<addr>: <the load>; b <back>) and replaces it with
## `b <veneer>`. Those are plain branches, not calls: X16/X17 may be live across
## them, so BOLT must not treat the pair as tail calls (whose stubs use X16).

# RUN: llvm-mc -filetype=obj -triple aarch64-unknown-linux-gnu %s -o %t.o
# RUN: %clang %cflags %t.o -o %t.exe -nostdlib -fuse-ld=lld -Wl,-q \
# RUN:   -Wl,--fix-cortex-a53-843419 -Wl,-z,separate-code
# RUN: llvm-objdump -d %t.exe | FileCheck --check-prefix=CHECK-INPUT %s
# RUN: llvm-bolt %t.exe -o %t.bolt --lite=0 2>&1 | FileCheck --check-prefix=CHECK-BOLT %s
# RUN: llvm-objdump -d -j .text %t.bolt | FileCheck --check-prefix=CHECK-OUTPUT %s

## The linker applied the workaround: the load sits in a veneer.
# CHECK-INPUT: <patched>:
# CHECK-INPUT-NEXT:  adrp x16
# CHECK-INPUT-NEXT:  adrp x0
# CHECK-INPUT-NEXT:  ldr x3
# CHECK-INPUT-NEXT:  b {{.*}} <__CortexA53843419_
# CHECK-INPUT: <__CortexA53843419_{{.*}}>:
# CHECK-INPUT-NEXT:  ldr x1, [x0, #0x{{[0-9a-f]+}}]
# CHECK-INPUT-NEXT:  b {{.*}}

# CHECK-BOLT: BOLT-INFO: number of Cortex-A53 erratum 843419 veneers folded back into their functions: 1

## After BOLT the function is straight-line again: no branch between the ADRP
## that defines x16 and its use, and the load is back in place.
# CHECK-OUTPUT: <patched>:
# CHECK-OUTPUT-NEXT:  adrp x16
# CHECK-OUTPUT-NEXT:  adrp x0
# CHECK-OUTPUT-NEXT:  ldr x3, [x16, #0x{{[0-9a-f]+}}]
# CHECK-OUTPUT-NEXT:  ldr x1, [x0, #0x{{[0-9a-f]+}}]
# CHECK-OUTPUT-NEXT:  ldr x2, [x16, #0x{{[0-9a-f]+}}]
# CHECK-OUTPUT-NEXT:  add x0, x1, x2
# CHECK-OUTPUT-NEXT:  ret

  .text
  .globl _start
  .type _start, %function
_start:
  bl patched
  ret
  .size _start, .-_start

## Put the ADRP at the last word of a 4KiB page (offset 0xffc), followed by a
## 64-bit load using its result within the next two instructions: that is the
## sequence erratum 843419 concerns and that --fix-cortex-a53-843419 patches.
  .balign 4096
  .space 4096 - 8
  .globl patched
  .type patched, %function
patched:
  adrp x16, data2               // x16 live across the patched sequence
  adrp x0, data1                // at page offset 0xffc
  ldr x3, [x16, :lo12:data2]    // erratum sequence: a load/store, then...
  ldr x1, [x0, :lo12:data1]     // ...a load off the ADRP: the linker moves this one to a veneer
  ldr x2, [x16, :lo12:data2]
  add x0, x1, x2
  ret
  .size patched, .-patched

  .data
  .balign 8
  .globl data1
data1:
  .quad 1
  .space 4096
  .globl data2
data2:
  .quad 2

## Force relocation mode.
  .reloc 0, R_AARCH64_NONE
