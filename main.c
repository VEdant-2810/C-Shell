// Minimal POSIX shell - entry point
#include "include/shell.h"

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    shell_loop();
    return 0;
}
