@ Guest-side ARM EABI runtime helpers, run INSIDE the emulated CPU (Cortex-A15 model:
@ VFPv4 + hardware integer divide), so the game's millions of soft-float calls per second
@ no longer leave the emulator. The game is soft-float code and never touches VFP
@ registers, so s0-s7 / d0-d3 are free scratch.
@
@ Semantics match the ARM RTABI and the C++ trap versions (src/hle/libc_core.cpp):
@   - IEEE round-to-nearest (FPSCR = 0: no flush-to-zero, no default-NaN)
@   - float->int conversions truncate and saturate, NaN -> 0 (VCVT without R)
@   - comparisons return 0 for unordered (NaN) operands
@   - integer division by zero returns 0 (SDIV/UDIV with SCTLR.DZ = 0)
@
@ Assembled by tools/gen_fastpath.py into src/cpu/fastpath_code.inc.

    .syntax unified
    .arm
    .arch armv7-a
    .arch_extension idiv
    .fpu vfpv4
    .text
    .align 2

    .macro FUNC name
    .global \name
    .type \name, %function
\name:
    .endm

@ ---- single precision ---------------------------------------------------------------------
    .macro FBINOP name, op
FUNC \name
    vmov s0, r0
    vmov s1, r1
    \op s0, s0, s1
    vmov r0, s0
    bx lr
    .endm
    FBINOP __aeabi_fadd, vadd.f32
    FBINOP __aeabi_fsub, vsub.f32
    FBINOP __aeabi_fmul, vmul.f32
    FBINOP __aeabi_fdiv, vdiv.f32

    .macro FCMP name, yes, no
FUNC \name
    vmov s0, r0
    vmov s1, r1
    vcmp.f32 s0, s1
    vmrs APSR_nzcv, fpscr
    mov\yes r0, #1
    mov\no r0, #0
    bx lr
    .endm
    FCMP __aeabi_fcmpeq, eq, ne
    FCMP __aeabi_fcmplt, mi, pl
    FCMP __aeabi_fcmple, ls, hi
    FCMP __aeabi_fcmpge, ge, lt
    FCMP __aeabi_fcmpgt, gt, le

FUNC __aeabi_f2iz
    vmov s0, r0
    vcvt.s32.f32 s0, s0
    vmov r0, s0
    bx lr

FUNC __aeabi_f2d
    vmov s0, r0
    vcvt.f64.f32 d1, s0
    vmov r0, r1, d1
    bx lr

FUNC __aeabi_i2f
    vmov s0, r0
    vcvt.f32.s32 s0, s0
    vmov r0, s0
    bx lr

FUNC __aeabi_ui2f
    vmov s0, r0
    vcvt.f32.u32 s0, s0
    vmov r0, s0
    bx lr

@ ---- double precision -----------------------------------------------------------------------
    .macro DBINOP name, op
FUNC \name
    vmov d0, r0, r1
    vmov d1, r2, r3
    \op d0, d0, d1
    vmov r0, r1, d0
    bx lr
    .endm
    DBINOP __aeabi_dadd, vadd.f64
    DBINOP __aeabi_dsub, vsub.f64
    DBINOP __aeabi_dmul, vmul.f64
    DBINOP __aeabi_ddiv, vdiv.f64

    .macro DCMP name, yes, no
FUNC \name
    vmov d0, r0, r1
    vmov d1, r2, r3
    vcmp.f64 d0, d1
    vmrs APSR_nzcv, fpscr
    mov\yes r0, #1
    mov\no r0, #0
    bx lr
    .endm
    DCMP __aeabi_dcmpeq, eq, ne
    DCMP __aeabi_dcmplt, mi, pl
    DCMP __aeabi_dcmple, ls, hi
    DCMP __aeabi_dcmpge, ge, lt
    DCMP __aeabi_dcmpgt, gt, le
    DCMP __aeabi_dcmpun, vs, vc

FUNC __aeabi_d2f
    vmov d0, r0, r1
    vcvt.f32.f64 s4, d0
    vmov r0, s4
    bx lr

FUNC __aeabi_d2iz
    vmov d0, r0, r1
    vcvt.s32.f64 s4, d0
    vmov r0, s4
    bx lr

FUNC __aeabi_d2uiz
    vmov d0, r0, r1
    vcvt.u32.f64 s4, d0
    vmov r0, s4
    bx lr

FUNC __aeabi_i2d
    vmov s0, r0
    vcvt.f64.s32 d1, s0
    vmov r0, r1, d1
    bx lr

FUNC __aeabi_ui2d
    vmov s0, r0
    vcvt.f64.u32 d1, s0
    vmov r0, r1, d1
    bx lr

@ ---- integer division / 64-bit multiply -------------------------------------------------------
FUNC __aeabi_idiv
    sdiv r0, r0, r1
    bx lr

FUNC __aeabi_uidiv
    udiv r0, r0, r1
    bx lr

FUNC __aeabi_idivmod
    sdiv r2, r0, r1
    mls r1, r2, r1, r0
    mov r0, r2
    bx lr

FUNC __aeabi_uidivmod
    udiv r2, r0, r1
    mls r1, r2, r1, r0
    mov r0, r2
    bx lr

@ (r1:r0) * (r3:r2), low 64 bits
FUNC __aeabi_lmul
    mul r3, r0, r3
    mla r1, r2, r1, r3
    umull r12, r2, r0, r2
    add r1, r1, r2
    mov r0, r12
    bx lr

@ ---- libm ---------------------------------------------------------------------------------------
FUNC sqrtf
    vmov s0, r0
    vsqrt.f32 s0, s0
    vmov r0, s0
    bx lr

FUNC sqrt
    vmov d0, r0, r1
    vsqrt.f64 d0, d0
    vmov r0, r1, d0
    bx lr
