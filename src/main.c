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
    int status;

    mc_init_context(&ctx);
    mc_print_banner();
    status = mc_run_cli(&ctx, argc, argv);

    /*
     * Jika masih ada snapshot aktif saat program berakhir, target harus
     * dilepas kembali agar tidak tertinggal dalam keadaan stop.
     */
    if (mc_release_snapshot(&ctx, true) != 0 && status == 0) {
        status = 1;
    }

    return status;
}
