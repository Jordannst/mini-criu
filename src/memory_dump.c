#include "mini_criu.h"

#include <stdio.h>
#include <string.h>

int mc_dump_memory(mc_context *ctx)
{
    char timestamp[32];
    char directory_name[128];
    char checkpoint_dir[PATH_MAX];
    char metadata_path[PATH_MAX];
    char metadata[512];

    if (ctx->target_pid <= 0) {
        mc_log_error("Belum ada target yang dipilih. Gunakan 'set-target <pid>' terlebih dahulu.");
        return 1;
    }

    if (!mc_process_exists(ctx->target_pid)) {
        mc_log_error("PID target yang dipilih sedang tidak berjalan.");
        return 1;
    }

    if (mc_ensure_directory(ctx->checkpoint_root) != 0) {
        mc_log_error("Gagal membuat direktori root checkpoint.");
        return 1;
    }

    mc_format_timestamp(timestamp, sizeof(timestamp));
    snprintf(directory_name, sizeof(directory_name), "checkpoint-pid-%d-%s", ctx->target_pid, timestamp);

    if (mc_join_path(checkpoint_dir, sizeof(checkpoint_dir), ctx->checkpoint_root, directory_name) != 0) {
        mc_log_error("Path checkpoint terlalu panjang.");
        return 1;
    }

    if (mc_ensure_directory(checkpoint_dir) != 0) {
        mc_log_error("Gagal membuat direktori checkpoint.");
        return 1;
    }

    if (mc_join_path(metadata_path, sizeof(metadata_path), checkpoint_dir, "checkpoint.info") != 0) {
        mc_log_error("Path metadata terlalu panjang.");
        return 1;
    }

    snprintf(metadata,
             sizeof(metadata),
             "scaffold checkpoint mini-criu\n"
             "target_pid=%d\n"
             "created_at=%s\n"
             "status=placeholder\n"
             "notes=Checkpoint ini hanya berisi metadata. Pengambilan memori/register belum diimplementasikan.\n",
             ctx->target_pid,
             timestamp);

    if (mc_write_text_file(metadata_path, metadata) != 0) {
        mc_log_error("Gagal menulis metadata checkpoint.");
        return 1;
    }

    snprintf(ctx->last_checkpoint_dir, sizeof(ctx->last_checkpoint_dir), "%s", checkpoint_dir);

    printf("Scaffold checkpoint dibuat: %s\n", checkpoint_dir);
    puts("TODO: parse /proc/<pid>/maps dan identifikasi region memori yang bisa di-dump.");
    puts("TODO: baca byte dari /proc/<pid>/mem dan serialisasikan ke dalam checkpoint.");

    return 0;
}
