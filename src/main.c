#include "mini_criu.h"

/*
 * Entry point program.
 *
 * Fungsi ini hanya menyiapkan konteks awal aplikasi, menampilkan banner, lalu
 * menyerahkan kontrol ke mode CLI.
 */
int main(int argc, char **argv)
{
    mc_context ctx;

    mc_init_context(&ctx);
    mc_print_banner();

    return mc_run_cli(&ctx, argc, argv);
}
