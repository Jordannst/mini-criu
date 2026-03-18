#include "mini_criu.h"

#include <stdio.h>

/*
 * Menangani permintaan restore dari direktori checkpoint tertentu.
 *
 * Saat ini fungsi ini masih berupa fondasi. Tugasnya baru memvalidasi bahwa
 * direktori checkpoint ada, menyimpannya ke konteks, lalu memberi tahu
 * pengguna langkah besar yang nantinya akan diisi.
 */
int mc_restore_checkpoint(mc_context *ctx, const char *checkpoint_dir)
{
    /*
     * Restore tidak boleh dilanjutkan jika folder checkpoint yang diminta
     * tidak tersedia.
     */
    if (!mc_directory_exists(checkpoint_dir)) {
        mc_log_error("Direktori checkpoint tidak ada.");
        return 1;
    }

    /*
     * Path checkpoint terakhir disimpan ke konteks agar status CLI dan command
     * berikutnya bisa mengetahui direktori yang sedang dipakai.
     */
    snprintf(ctx->last_checkpoint_dir, sizeof(ctx->last_checkpoint_dir), "%s", checkpoint_dir);

    /*
     * Bagian ini sengaja hanya menjelaskan alur besar restore. Implementasi
     * nyata baru akan diisi setelah format checkpoint dianggap cukup stabil.
     */
    printf("Permintaan restore dari: %s\n", checkpoint_dir);
    puts("Alur restore saat ini masih berupa scaffold.");
    puts("TODO: baca metadata checkpoint dan validasi formatnya.");
    puts("TODO: bangun ulang state proses, pulihkan register, dan bangun ulang memory mapping.");

    return 0;
}
