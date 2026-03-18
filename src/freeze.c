#include "mini_criu.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>

/*
 * Menyiapkan direktori output untuk hasil freeze.
 *
 * Setiap command `freeze` yang berhasil harus membuka snapshot baru. Karena itu,
 * direktori checkpoint selalu dibuat baru agar satu folder mewakili satu event
 * snapshot yang jelas.
 */
static int mc_prepare_freeze_checkpoint_dir(mc_context *ctx,
                                            const char *timestamp,
                                            char *checkpoint_dir,
                                            size_t size)
{
    char directory_name[128];
    int written;

    if (mc_ensure_directory(ctx->checkpoint_root) != 0) {
        mc_log_error("Gagal membuat direktori root checkpoint.");
        return -1;
    }

    written = snprintf(directory_name,
                       sizeof(directory_name),
                       "checkpoint-pid-%d-%s",
                       ctx->target_pid,
                       timestamp);
    if (written < 0 || (size_t)written >= sizeof(directory_name)) {
        mc_log_error("Nama direktori checkpoint terlalu panjang.");
        return -1;
    }

    if (mc_join_path(checkpoint_dir, size, ctx->checkpoint_root, directory_name) != 0) {
        mc_log_error("Path checkpoint terlalu panjang.");
        return -1;
    }

    if (mc_ensure_directory(checkpoint_dir) != 0) {
        mc_log_error("Gagal membuat direktori checkpoint.");
        return -1;
    }

    written = snprintf(ctx->last_checkpoint_dir, sizeof(ctx->last_checkpoint_dir), "%s", checkpoint_dir);
    if (written < 0 || (size_t)written >= sizeof(ctx->last_checkpoint_dir)) {
        mc_log_error("Path checkpoint terlalu panjang untuk disimpan di konteks.");
        return -1;
    }

    return 0;
}

/*
 * Menulis register CPU ke file teks agar mudah dibaca saat dipelajari.
 *
 * Format teks dipilih supaya isi file dapat langsung dibuka dan dijelaskan
 * tanpa perlu alat tambahan lain.
 */
static int mc_write_register_dump(const char *path,
                                  pid_t pid,
                                  const char *timestamp,
                                  const struct user_regs_struct *regs)
{
    FILE *file = fopen(path, "w");

    if (file == NULL) {
        mc_log_system_error("Gagal membuka file dump register");
        return -1;
    }

    if (fprintf(file,
                "jenis_dump=register_cpu_x86_64\n"
                "pid_target=%d\n"
                "dibuat_pada=%s\n"
                "r15=0x%llx\n"
                "r14=0x%llx\n"
                "r13=0x%llx\n"
                "r12=0x%llx\n"
                "rbp=0x%llx\n"
                "rbx=0x%llx\n"
                "r11=0x%llx\n"
                "r10=0x%llx\n"
                "r9=0x%llx\n"
                "r8=0x%llx\n"
                "rax=0x%llx\n"
                "rcx=0x%llx\n"
                "rdx=0x%llx\n"
                "rsi=0x%llx\n"
                "rdi=0x%llx\n"
                "orig_rax=0x%llx\n"
                "rip=0x%llx\n"
                "cs=0x%llx\n"
                "eflags=0x%llx\n"
                "rsp=0x%llx\n"
                "ss=0x%llx\n"
                "fs_base=0x%llx\n"
                "gs_base=0x%llx\n"
                "ds=0x%llx\n"
                "es=0x%llx\n"
                "fs=0x%llx\n"
                "gs=0x%llx\n",
                pid,
                timestamp,
                (unsigned long long)regs->r15,
                (unsigned long long)regs->r14,
                (unsigned long long)regs->r13,
                (unsigned long long)regs->r12,
                (unsigned long long)regs->rbp,
                (unsigned long long)regs->rbx,
                (unsigned long long)regs->r11,
                (unsigned long long)regs->r10,
                (unsigned long long)regs->r9,
                (unsigned long long)regs->r8,
                (unsigned long long)regs->rax,
                (unsigned long long)regs->rcx,
                (unsigned long long)regs->rdx,
                (unsigned long long)regs->rsi,
                (unsigned long long)regs->rdi,
                (unsigned long long)regs->orig_rax,
                (unsigned long long)regs->rip,
                (unsigned long long)regs->cs,
                (unsigned long long)regs->eflags,
                (unsigned long long)regs->rsp,
                (unsigned long long)regs->ss,
                (unsigned long long)regs->fs_base,
                (unsigned long long)regs->gs_base,
                (unsigned long long)regs->ds,
                (unsigned long long)regs->es,
                (unsigned long long)regs->fs,
                (unsigned long long)regs->gs) < 0) {
        fclose(file);
        mc_log_system_error("Gagal menulis dump register");
        return -1;
    }

    if (fclose(file) != 0) {
        mc_log_system_error("Gagal menutup file dump register");
        return -1;
    }

    return 0;
}

/*
 * Menulis metadata dasar hasil freeze.
 *
 * File ini menjelaskan bahwa register sudah berhasil diambil dan menunjukkan
 * file mana yang berisi dump register.
 */
static int mc_write_freeze_metadata(const char *path,
                                    pid_t pid,
                                    const char *snapshot_id,
                                    const char *timestamp,
                                    int stop_signal,
                                    const struct user_regs_struct *regs)
{
    char metadata[1024];
    int written;

    written = snprintf(metadata,
                       sizeof(metadata),
                       "jenis_checkpoint=freeze\n"
                       "snapshot_id=%s\n"
                       "pid_target=%d\n"
                       "dibuat_pada=%s\n"
                       "status=register_berhasil_diambil\n"
                       "status_snapshot=aktif_menunggu_dump_memori\n"
                       "sinyal_stop=%d\n"
                       "file_register=regs.dump\n"
                       "rip_awal=0x%llx\n"
                       "rsp_awal=0x%llx\n"
                       "dump_memori=menunggu_command_dump-memory\n"
                       "restore=belum_diimplementasikan\n"
                       "catatan=Target tetap dihentikan setelah freeze agar dump register dan dump memori bisa berasal dari snapshot yang sama selama sesi CLI ini.\n",
                       snapshot_id,
                       pid,
                       timestamp,
                       stop_signal,
                       (unsigned long long)regs->rip,
                       (unsigned long long)regs->rsp);
    if (written < 0 || (size_t)written >= sizeof(metadata)) {
        mc_log_error("Metadata freeze terlalu panjang untuk ditulis.");
        return -1;
    }

    if (mc_write_text_file(path, metadata) != 0) {
        mc_log_system_error("Gagal menulis metadata freeze");
        return -1;
    }

    return 0;
}

int mc_freeze_target(mc_context *ctx)
{
    struct user_regs_struct regs;
    char timestamp[32];
    char checkpoint_dir[PATH_MAX];
    char regs_path[PATH_MAX];
    char metadata_path[PATH_MAX];
    int wait_status = 0;
    int stop_signal = 0;
    int result = 1;
    bool attached = false;
    bool stopped = false;

    /*
     * Validasi awal mencegah ptrace dijalankan pada PID yang belum dipilih atau
     * proses yang sudah tidak ada.
     */
    if (ctx->snapshot_active) {
        mc_log_error("Masih ada snapshot aktif. Jalankan 'dump-memory' untuk menyelesaikannya atau keluar dari CLI untuk membatalkannya.");
        return 1;
    }

    if (ctx->target_pid <= 0) {
        mc_log_error("Belum ada target yang dipilih. Gunakan 'set-target <pid>' terlebih dahulu.");
        return 1;
    }

    if (!mc_process_exists(ctx->target_pid)) {
        mc_log_error("PID target yang dipilih sedang tidak berjalan.");
        return 1;
    }

    printf("Memulai freeze untuk PID %d.\n", ctx->target_pid);

    /*
     * Attach membuat target masuk ke mode trace. Setelah itu kita wajib menunggu
     * dengan waitpid agar target benar-benar berhenti sebelum register dibaca
     * atau file checkpoint ditulis.
     */
    if (ptrace(PTRACE_ATTACH, ctx->target_pid, NULL, NULL) == -1) {
        if (errno == EPERM) {
            mc_log_error("Gagal melakukan ptrace attach ke target. Target mungkin belum mengizinkan tracing dari proses ini.");
        } else {
            mc_log_system_error("Gagal melakukan ptrace attach ke target");
        }
        return 1;
    }
    attached = true;

    puts("Attach berhasil. Menunggu target berhenti...");
    if (waitpid(ctx->target_pid, &wait_status, 0) == -1) {
        mc_log_system_error("Gagal menunggu target berhenti");
        goto cleanup;
    }

    if (!WIFSTOPPED(wait_status)) {
        mc_log_error("Target tidak masuk ke status stop yang diharapkan.");
        goto cleanup;
    }
    stopped = true;
    stop_signal = WSTOPSIG(wait_status);

    printf("Target berhenti dengan sinyal %d.\n", stop_signal);

    /*
     * Setelah target berhenti, register CPU dapat diambil dengan aman melalui
     * PTRACE_GETREGS. Data ini menjadi snapshot register yang paling awal dan
     * paling kecil untuk disimpan.
     */
    if (ptrace(PTRACE_GETREGS, ctx->target_pid, NULL, &regs) == -1) {
        mc_log_system_error("Gagal mengambil register CPU dari target");
        goto cleanup;
    }

    mc_format_timestamp(timestamp, sizeof(timestamp));

    /*
     * Setelah register tersedia, direktori checkpoint disiapkan dan dua file
     * awal ditulis:
     * - `regs.dump` untuk isi register
     * - `checkpoint.info` untuk ringkasan checkpoint
     */
    if (mc_prepare_freeze_checkpoint_dir(ctx, timestamp, checkpoint_dir, sizeof(checkpoint_dir)) != 0) {
        goto cleanup;
    }

    if (mc_join_path(regs_path, sizeof(regs_path), checkpoint_dir, "regs.dump") != 0) {
        mc_log_error("Path file register terlalu panjang.");
        goto cleanup;
    }

    if (mc_join_path(metadata_path, sizeof(metadata_path), checkpoint_dir, "checkpoint.info") != 0) {
        mc_log_error("Path file metadata terlalu panjang.");
        goto cleanup;
    }

    if (mc_write_register_dump(regs_path, ctx->target_pid, timestamp, &regs) != 0) {
        goto cleanup;
    }

    if (mc_write_freeze_metadata(metadata_path, ctx->target_pid, timestamp, timestamp, stop_signal, &regs) != 0) {
        goto cleanup;
    }

    /*
     * Snapshot dinyatakan aktif setelah file register dan metadata awal selesai
     * ditulis. Mulai titik ini target sengaja tetap dalam keadaan stop sampai
     * `dump-memory` selesai atau sesi CLI berakhir.
     */
    snprintf(ctx->active_snapshot_id, sizeof(ctx->active_snapshot_id), "%s", timestamp);
    ctx->snapshot_active = true;
    attached = false;

    printf("Freeze awal berhasil. Data disimpan di: %s\n", checkpoint_dir);
    puts("File yang dihasilkan: checkpoint.info dan regs.dump.");
    puts("Selama sesi CLI ini, target tetap dihentikan agar command 'dump-memory' dapat melanjutkan snapshot yang sama.");
    puts("Catatan: jika sesi CLI berakhir sebelum dump-memory dijalankan, target akan dilepas kembali otomatis.");
    result = 0;

cleanup:
    /*
     * Jika freeze gagal sebelum snapshot aktif terbentuk, target harus dilepas
     * kembali agar proses tidak tertahan dalam keadaan stop.
     */
    if (attached) {
        if (ctx->snapshot_active) {
            if (mc_release_snapshot(ctx, true) != 0) {
                result = 1;
            }
        } else if (ptrace(PTRACE_DETACH, ctx->target_pid, NULL, NULL) == -1) {
            mc_log_system_error("Gagal melepaskan ptrace dari target");
            result = 1;
        } else if (stopped) {
            puts("Target dilepas kembali dan diizinkan berjalan.");
        }
    }

    return result;
}
