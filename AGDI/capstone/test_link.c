/* Link + functional test for the static capstone.lib.
 *
 * Proves three things at once:
 *   1. the static library links into an x86 /MT executable (no capstone.dll)
 *   2. cs_open(CS_ARCH_ARM, THUMB | MCLASS) works
 *   3. cs_disasm gives the size and the instruction id the scan needs
 *
 * The byte sequence is Thumb little-endian halfwords:
 *   0xB5F0  PUSH {r4-r6,lr}
 *   0xBF00  NOP
 *   0x4770  BX lr
 */
#include <stdio.h>
#include <capstone/capstone.h>

int main(void)
{
    csh h;
    cs_err e = cs_open(CS_ARCH_ARM, CS_MODE_THUMB | CS_MODE_MCLASS, &h);
    if (e != CS_ERR_OK) {
        printf("cs_open FAILED: %d (%s)\n", (int)e, cs_strerror(e));
        return 1;
    }
    unsigned v = cs_version(NULL, NULL);
    printf("cs_open OK  capstone %u.%u\n", v >> 8, v & 0xFF);

    if (cs_option(h, CS_OPT_DETAIL, CS_OPT_ON) != CS_ERR_OK) {
        printf("cs_option FAILED\n");
        cs_close(&h);
        return 1;
    }

    static const unsigned char code[] = { 0xF0, 0xB5, 0x00, 0xBF, 0x70, 0x47 };

    cs_insn *insn = NULL;
    size_t n = cs_disasm(h, code, sizeof(code), 0x080007B8, 0, &insn);
    printf("disassembled %u instruction(s)\n", (unsigned)n);
    for (size_t i = 0; i < n; ++i) {
        printf("  0x%08X  %-8s %-16s size=%u id=%u\n",
               (unsigned)insn[i].address, insn[i].mnemonic, insn[i].op_str,
               insn[i].size, (unsigned)insn[i].id);
    }

    /* Only the two ids the scanner actually keys on are asserted. 0xBF00 is the
     * architectural NOP, but capstone maps it to its hint encoding (id 63), not
     * ARM_INS_NOP - so asserting ARM_INS_NOP there tests capstone's table rather
     * than our use of it. */
    printf("expected ids: PUSH=%u BX=%u (NOP decoded as %u)\n",
           (unsigned)ARM_INS_PUSH, (unsigned)ARM_INS_BX, (unsigned)insn[1].id);

    int pass = (n == 3 &&
                insn[0].id == ARM_INS_PUSH &&
                insn[2].id == ARM_INS_BX &&
                insn[0].size == 2 && insn[1].size == 2 && insn[2].size == 2);

    cs_free(insn, n);
    cs_close(&h);

    printf(pass ? "LINK TEST PASSED\n" : "LINK TEST FAILED\n");
    return pass ? 0 : 1;
}
