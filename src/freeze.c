#include "mini_criu.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>

/*
 * Menyiapkan direktori output untuk fase freeze.
 *
 * Jika konteks sudah memiliki checkpoint terakhir yang masih ada, direktori itu
 * digunakan kembali agar fase berikutnya dapat menambahkan file lain ke lokasi
 * yang sama. Jika belum ada, fungsi ini membuat direktori checkpoint baru.
 */
static int mc_prepare_freeze_checkpoint_dir(mc_context *ctx,
                                            const char *timestamp,
                                            char *checkpoint_dir,
                                            size_t size)
{
    char directory_name[128];
    int written;

    if (ctx->last_checkpoint_dir[0] != '\0' && mc_directory_exists(ctx->last_checkpoint_dir)) {
        written = snprintf(checkpoint_dir, size, "%s", ctx->last_checkpoint_dir);
        if (written < 0 || (size_t)written >= size) {
            mc_log_error("Path checkpoint terakhir terlalu panjang.");
            return -1;
        }
        return 0;
    }

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
 * tanpa perlu alat tambahan pada fase awal proyek ini.
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
 * Menulis metadata checkpoint awal yang bermakna.
 *
 * Pada fase ini metadata belum menyimpan peta memori atau isi memori proses.
 * File ini hanya mendeskripsikan hasil freeze awal dan lokasi dump register.
 */
static int mc_write_freeze_metadata(const char *path,
                                    pid_t pid,
                                    const char *timestamp,
                                    int stop_signal,
                                    const struct user_regs_struct *regs)
{
    char metadata[1024];
    int written;

    written = snprintf(metadata,
                       sizeof(metadata),
                       "fase=freeze\n"
                       "pid_target=%d\n"
                       "dibuat_pada=%s\n"
                       "status=register_berhasil_diambil\n"
                       "sinyal_stop=%d\n"
                       "file_register=regs.dump\n"
                       "rip_awal=0x%llx\n"
                       "rsp_awal=0x%llx\n"
                       "dump_memori=belum_diimplementasikan\n"
                       "restore=belum_diimplementasikan\n"
                       "catatan=Fase ini hanya mencakup attach, sinkronisasi stop, dan dump register awal.\n",
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
     * Validasi awal menjaga agar ptrace hanya dijalankan untuk PID yang memang
     * sudah dipilih dan masih terlihat oleh proses mini-criu.
     */
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
     * dengan waitpid agar target benar-benar berhenti sebelum register dibaca.
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
     * PTRACE_GETREGS sebagai data checkpoint awal yang paling kecil.
     */
    if (ptrace(PTRACE_GETREGS, ctx->target_pid, NULL, &regs) == -1) {
        mc_log_system_error("Gagal mengambil register CPU dari target");
        goto cleanup;
    }

    mc_format_timestamp(timestamp, sizeof(timestamp));

    /*
     * Direktori checkpoint dibuat atau digunakan kembali setelah register berhasil
     * diambil, lalu data awal ditulis ke file yang mudah diperiksa.
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

    if (mc_write_freeze_metadata(metadata_path, ctx->target_pid, timestamp, stop_signal, &regs) != 0) {
        goto cleanup;
    }

    printf("Freeze awal berhasil. Data disimpan di: %s\n", checkpoint_dir);
    puts("File yang dihasilkan: checkpoint.info dan regs.dump.");
    puts("TODO: fase berikutnya akan menambahkan dump memori. Restore belum diimplementasikan.");
    result = 0;

cleanup:
    /*
     * Pada fase ini target dilepas kembali agar proses dapat melanjutkan eksekusi.
     * Nanti, saat dump memori ditambahkan, alur ini bisa diubah agar target tetap
     * berhenti sampai seluruh data checkpoint selesai diambil.
     */
    if (attached) {
        if (ptrace(PTRACE_DETACH, ctx->target_pid, NULL, NULL) == -1) {
            mc_log_system_error("Gagal melepaskan ptrace dari target");
            result = 1;
        } else if (stopped) {
            puts("Target dilepas kembali dan diizinkan berjalan.");
        }
    }

    return result;
}
