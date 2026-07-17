#!/usr/bin/env python3
# MainDOB — regenerate kernel/arch/x86/vec_stubs.h
#
# The generic vector trampolines for the IOAPIC/MSI allocatable range are
# emitted by a VEC_STUB loop in arch/x86/isr_stub.asm. C cannot take the
# address of an assembler global without naming it, so this script writes
# the matching externs and a vector->entrypoint address table.
#
# IMPORTANT: VEC_STUB_FIRST/LAST below MUST match the %assign/%rep range
# in isr_stub.asm. Change both together, then re-run:
#     python3 tools/gen_vec_stubs.py
#
# Output is deterministic; do not hand-edit the generated header.

LO, HI = 0x50, 0xDF   # inclusive; must match isr_stub.asm VEC_STUB loop
OUT = "kernel/arch/x86/vec_stubs.h"


def main():
    L = []
    L.append("/* MainDOB — generic vector trampoline externs + address table.")
    L.append(" *")
    L.append(" * GENERATED to match the VEC_STUB loop in arch/x86/isr_stub.asm.")
    L.append(" * The assembler emits one global `vec_stub_<decimal>` per vector in")
    L.append(" * [0x%02X, 0x%02X]; C cannot take their addresses without naming each," % (LO, HI))
    L.append(" * so this header declares them and assembles a vector->entrypoint table.")
    L.append(" * Regenerate (do not hand-edit) if the VEC_STUB range changes:")
    L.append(" *   tools/gen_vec_stubs.py  (range constants must match isr_stub.asm)")
    L.append(" *")
    L.append(" * Indices below 0x%02X or above 0x%02X are 0 (no trampoline): those" % (LO, HI))
    L.append(" * vectors are CPU exceptions, legacy PIC IRQs, the syscall gate, or")
    L.append(" * LAPIC timer/spurious, none of which are dynamically allocatable. */")
    L.append("")
    L.append("#ifndef MAINDOB_ARCH_X86_VEC_STUBS_H")
    L.append("#define MAINDOB_ARCH_X86_VEC_STUBS_H")
    L.append("")
    L.append("#include \"lib/types.h\"")
    L.append("")
    L.append("#define VEC_STUB_FIRST  0x%02X" % LO)
    L.append("#define VEC_STUB_LAST   0x%02X" % HI)
    L.append("")
    for v in range(LO, HI + 1):
        L.append("extern void vec_stub_%d(void);" % v)
    L.append("")
    L.append("/* Indexed by vector number (0..255). Non-allocatable vectors are NULL. */")
    L.append("static void (* const vector_trampoline_table[256])(void) =")
    L.append("{")
    for v in range(LO, HI + 1):
        L.append("    [0x%02X] = vec_stub_%d," % (v, v))
    L.append("};")
    L.append("")
    L.append("#endif /* MAINDOB_ARCH_X86_VEC_STUBS_H */")
    with open(OUT, "w") as f:
        f.write("\n".join(L) + "\n")
    print("wrote %s (%d trampolines)" % (OUT, HI - LO + 1))


if __name__ == "__main__":
    main()
