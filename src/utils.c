#include "mini_criu.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/*
 * Mengisi konteks awal aplikasi.
 *
 * Nilai default ini dipakai oleh seluruh command selama sesi CLI berjalan.
 */
void mc_init_context(mc_context *ctx)
{
    ctx->target_pid = -1;
    ctx->running = false;
    ctx->snapshot_active = false;
    snprintf(ctx->checkpoint_root, sizeof(ctx->checkpoint_root), "%s", MC_DEFAULT_CHECKPOINT_ROOT);
    ctx->last_checkpoint_dir[0] = '\0';
    ctx->active_snapshot_id[0] = '\0';
}

/*
 * Menghapus karakter akhir baris dari input `fgets`.
 *
 * Ini membantu parser command bekerja pada teks yang sudah bersih.
 */
void mc_trim_newline(char *line)
{
    size_t len = strlen(line);

    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
        line[--len] = '\0';
    }
}

/*
 * Menghapus spasi dan tab di awal/akhir string.
 *
 * Fungsi ini dipakai sebelum tokenisasi command agar input lebih konsisten.
 */
char *mc_trim_whitespace(char *text)
{
    char *end;

    while (*text == ' ' || *text == '\t') {
        ++text;
    }

    if (*text == '\0') {
        return text;
    }

    end = text + strlen(text) - 1;
    while (end > text && (*end == ' ' || *end == '\t')) {
        *end-- = '\0';
    }

    return text;
}

/*
 * Mengubah teks menjadi PID yang valid.
 *
 * Hanya bilangan bulat positif yang diterima sebagai PID target.
 */
bool mc_parse_pid(const char *text, pid_t *pid_out)
{
    char *end = NULL;
    long value;

    errno = 0;
    value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value <= 0) {
        return false;
    }

    *pid_out = (pid_t)value;
    return true;
}

bool mc_process_exists(pid_t pid)
{
    if (pid <= 0) {
        return false;
    }

    if (kill(pid, 0) == 0) {
        return true;
    }

    return errno == EPERM;
}

/*
 * Mengecek apakah path yang diberikan adalah direktori.
 */
bool mc_directory_exists(const char *path)
{
    struct stat st;

    if (stat(path, &st) != 0) {
        return false;
    }

    return S_ISDIR(st.st_mode);
}

/*
 * Membuat direktori jika belum ada.
 *
 * Fungsi ini dipakai untuk root checkpoint dan folder checkpoint hasil dump.
 */
int mc_ensure_directory(const char *path)
{
    if (mc_directory_exists(path)) {
        return 0;
    }

    if (mkdir(path, 0755) == 0) {
        return 0;
    }

    return errno == EEXIST ? 0 : -1;
}

/*
 * Menulis seluruh teks ke file.
 *
 * Helper ini dipakai untuk file metadata sederhana yang isinya berupa teks.
 */
int mc_write_text_file(const char *path, const char *contents)
{
    FILE *file = fopen(path, "w");

    if (file == NULL) {
        return -1;
    }

    if (fputs(contents, file) == EOF) {
        fclose(file);
        return -1;
    }

    return fclose(file);
}

/*
 * Menggabungkan dua bagian path dengan separator `/`.
 */
int mc_join_path(char *buffer, size_t size, const char *left, const char *right)
{
    int written = snprintf(buffer, size, "%s/%s", left, right);

    if (written < 0 || (size_t)written >= size) {
        return -1;
    }

    return 0;
}

/*
 * Membuat timestamp singkat untuk nama direktori dan metadata checkpoint.
 */
void mc_format_timestamp(char *buffer, size_t size)
{
    time_t now = time(NULL);
    struct tm tm_now;

    localtime_r(&now, &tm_now);
    strftime(buffer, size, "%Y%m%d-%H%M%S", &tm_now);
}

/*
 * Helper log sederhana untuk pesan informasional.
 */
void mc_log_info(const char *message)
{
    fprintf(stdout, "[info] %s\n", message);
}

/*
 * Helper log sederhana untuk galat yang tidak membutuhkan `errno`.
 */
void mc_log_error(const char *message)
{
    fprintf(stderr, "[galat] %s\n", message);
}

/*
 * Helper log untuk galat sistem yang memiliki pesan dari `errno`.
 */
void mc_log_system_error(const char *message)
{
    int saved_errno = errno;

    fprintf(stderr, "[galat] %s: %s\n", message, strerror(saved_errno));
}

/*
 * Melepaskan target yang masih ditahan oleh snapshot aktif.
 *
 * Helper ini dipakai saat `dump-memory` selesai dan juga saat program keluar,
 * sehingga perilaku stop/resume target tetap mudah dipahami.
 */
int mc_release_snapshot(mc_context *ctx, bool announce)
{
    if (!ctx->snapshot_active) {
        return 0;
    }

    if (ctx->target_pid > 0 && ptrace(PTRACE_DETACH, ctx->target_pid, NULL, NULL) == -1) {
        if (errno != ESRCH) {
            mc_log_system_error("Gagal melepaskan ptrace dari target");
            return -1;
        }

        if (announce) {
            puts("Snapshot aktif dibersihkan, tetapi target sudah tidak tersedia.");
        }
    } else if (announce) {
        puts("Target dilepas kembali dan diizinkan berjalan.");
    }

    ctx->snapshot_active = false;
    ctx->active_snapshot_id[0] = '\0';
    return 0;
}

/*
 * Menyimpan PID target ke konteks setelah memastikan prosesnya ada.
 */
int mc_set_target(mc_context *ctx, pid_t pid)
{
    /*
     * Mengganti target saat snapshot masih aktif akan membuat alur checkpoint
     * membingungkan karena proses lama masih ditahan dalam keadaan stop.
     */
    if (ctx->snapshot_active) {
        mc_log_error("Masih ada snapshot aktif. Jalankan 'dump-memory' untuk menyelesaikannya atau keluar dari CLI untuk membatalkannya.");
        return 1;
    }

    if (!mc_process_exists(pid)) {
        mc_log_error("PID target tidak berjalan atau tidak terlihat dari lingkungan ini.");
        return 1;
    }

    ctx->target_pid = pid;
    printf("PID target diatur ke %d.\n", pid);
    return 0;
}
