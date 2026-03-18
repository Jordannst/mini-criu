#include "mini_criu.h"

int main(int argc, char **argv)
{
    mc_context ctx;

    mc_init_context(&ctx);
    mc_print_banner();

    return mc_run_cli(&ctx, argc, argv);
}
