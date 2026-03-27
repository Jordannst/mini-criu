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
    status = mc_run_cli(&ctx, argc, argv);

    /*
     * Jika sesi berakhir saat snapshot masih aktif, target dilepas dari tracer
     * tetapi dibiarkan tetap stop agar kondisi freeze tidak hilang hanya karena
     * CLI ditutup.
     */
    if (mc_release_snapshot(&ctx, true, SIGSTOP) != 0 && status == 0) {
        status = 1;
    }

    return status;
}
