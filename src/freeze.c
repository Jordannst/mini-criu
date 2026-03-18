#include "mini_criu.h"

#include <stdio.h>

int mc_freeze_target(mc_context *ctx)
{
    if (ctx->target_pid <= 0) {
        mc_log_error("Belum ada target yang dipilih. Gunakan 'set-target <pid>' terlebih dahulu.");
        return 1;
    }

    if (!mc_process_exists(ctx->target_pid)) {
        mc_log_error("PID target yang dipilih sedang tidak berjalan.");
        return 1;
    }

    printf("Menyiapkan freeze untuk PID %d.\n", ctx->target_pid);
    puts("Tahap freeze saat ini masih berupa scaffold.");
    puts("TODO: lakukan attach dengan ptrace(PTRACE_ATTACH) dan tunggu proses berhenti.");
    puts("TODO: ambil register saat proses sudah berada pada status stop yang stabil.");

    return 0;
}
