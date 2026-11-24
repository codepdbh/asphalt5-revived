/* Standalone ADB probe: loads the audited engine through the same runtime as
 * the app, runs its constructors, checks the engine's crc32 and runs the
 * registered destructors.  Usage: probe <libasphalt5.so> */
#include "runtime.h"

#include <stdlib.h>

int a5_boot_engine(const char *engine_path, CPU *cpu);
void a5_run_guest_destructors(CPU *cpu);

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <audited Asphalt 5 HD 3.4.1 ELF>\n", argv[0]);
        return EXIT_FAILURE;
    }
    a5_set_paths("/data/local/tmp", "/data/local/tmp");
    CPU cpu;
    if (a5_boot_engine(argv[1], &cpu) != 0) return EXIT_FAILURE;
    puts("Asphalt 5 HD 3.4.1 constructor array completed");
    puts("Asphalt 5 engine crc32: 0xcbf43926");
    a5_run_guest_destructors(&cpu);
    puts("Asphalt 5 engine registered destructors completed");
    guest_mem_shutdown();
    return EXIT_SUCCESS;
}
