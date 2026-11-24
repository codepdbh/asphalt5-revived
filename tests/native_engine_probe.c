#include "functions.h"

#include <stdlib.h>

void guest_return_or_jump(CPU *cpu, uint32_t target, uint32_t entry_lr)
{
    if (target != entry_lr) {
        fprintf(stderr, "Unexpected guest branch to 0x%08x\n", target);
        abort();
    }
    cpu->r[15] = target;
}

void vfp_set_cmp32(CPU *cpu, float left, float right)
{
    uint32_t nzcv;
    if (isunordered(left, right)) {
        nzcv = 0x30000000u;
    } else if (left < right) {
        nzcv = 0x80000000u;
    } else if (left == right) {
        nzcv = 0x60000000u;
    } else {
        nzcv = 0x20000000u;
    }
    cpu->fpscr = (cpu->fpscr & 0x0fffffffu) | nzcv;
}

static uint32_t float_bits(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

int main(void)
{
    const struct {
        float value, minimum, maximum, expected;
    } cases[] = {
        {-3.0f, -2.0f, 4.0f, -2.0f},
        {1.25f, -2.0f, 4.0f, 1.25f},
        {8.0f, -2.0f, 4.0f, 4.0f},
        {-2.0f, -2.0f, 4.0f, -2.0f},
        {4.0f, -2.0f, 4.0f, 4.0f},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        CPU cpu = {0};
        cpu.r[0] = float_bits(cases[i].value);
        cpu.r[1] = float_bits(cases[i].minimum);
        cpu.r[2] = float_bits(cases[i].maximum);
        cpu.r[14] = 0x1000;
        fn_00062cd0(&cpu);
        if (cpu.r[0] != float_bits(cases[i].expected) || cpu.r[15] != cpu.r[14]) {
            fprintf(stderr, "Clamp case %zu failed (r0=0x%08x, pc=0x%08x)\n",
                    i, cpu.r[0], cpu.r[15]);
            return EXIT_FAILURE;
        }
    }
    puts("AArch64 AOT: 5 Asphalt 5 Clamp cases passed");
    return EXIT_SUCCESS;
}

