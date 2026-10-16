/* snesrecomp_codegen_host for platforms that ship prebuilt (consoles): there
 * is no CMake, Python or child process to regenerate and rebuild the game,
 * so the launcher's Generate & rebuild is simply not offered. */
#include "snesrecomp_codegen_host.h"

#include <stdio.h>
#include <stdlib.h>

void snesrecomp_codegen_host_apply(RecompLauncherCGameInfo *gi,
                                   const SnesrecompCodegenHostConfig *cfg)
{
    (void)gi; (void)cfg;
}

int snesrecomp_codegen_host_sources_missing(const SnesrecompCodegenHostConfig *cfg)
{
    (void)cfg;
    return 0;   /* the generated code is compiled in */
}

void snesrecomp_codegen_host_relaunch_or_exit(const char *rom_path)
{
    (void)rom_path;
    fprintf(stderr, "codegen host: relaunch is not available on this platform\n");
    exit(0);
}

void snesrecomp_codegen_host_autowire(RecompLauncherCGameInfo *gi, const char *display_name)
{
    (void)gi; (void)display_name;
}
