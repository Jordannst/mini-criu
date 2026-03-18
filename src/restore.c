#include "mini_criu.h"

#include <stdio.h>

int mc_restore_checkpoint(mc_context *ctx, const char *checkpoint_dir)
{
    if (!mc_directory_exists(checkpoint_dir)) {
        mc_log_error("Direktori checkpoint tidak ada.");
        return 1;
    }

    snprintf(ctx->last_checkpoint_dir, sizeof(ctx->last_checkpoint_dir), "%s", checkpoint_dir);

    printf("Permintaan restore dari: %s\n", checkpoint_dir);
    puts("Alur restore saat ini masih berupa scaffold.");
    puts("TODO: baca metadata checkpoint dan validasi formatnya.");
    puts("TODO: bangun ulang state proses, pulihkan register, dan bangun ulang memory mapping.");

    return 0;
}
