#include "mini_criu.h"

#include <dirent.h>
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

#define MC_FMT_RESET "\033[0m"
#define MC_FMT_BOLD_CYAN "\033[1;36m"
#define MC_FMT_GREEN "\033[32m"
#define MC_FMT_YELLOW "\033[33m"
#define MC_FMT_RED "\033[31m"
#define MC_FMT_DIM "\033[2m"

static bool mc_stream_supports_color(FILE *stream)
{
    return isatty(fileno(stream)) != 0;
}

static void mc_print_level(FILE *stream,
                           const char *tag,
                           const char *message,
                           const char *color_code)
{
    if (mc_stream_supports_color(stream)) {
        fprintf(stream, "%s[%s]%s %s\n", color_code, tag, MC_FMT_RESET, message);
        fflush(stream);
        return;
    }

    fprintf(stream, "[%s] %s\n", tag, message);
    fflush(stream);
}

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
    ctx->active_checkpoint_flag[0] = '\0';
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
    mc_print_level(stdout, "info", message, MC_FMT_BOLD_CYAN);
}

void mc_log_ok(const char *message)
{
    mc_print_level(stdout, "ok", message, MC_FMT_GREEN);
}

void mc_log_warn(const char *message)
{
    mc_print_level(stdout, "warn", message, MC_FMT_YELLOW);
}

/*
 * Helper log sederhana untuk galat yang tidak membutuhkan `errno`.
 */
void mc_log_error(const char *message)
{
    mc_print_level(stderr, "err", message, MC_FMT_RED);
}

/*
 * Helper log untuk galat sistem yang memiliki pesan dari `errno`.
 */
void mc_log_system_error(const char *message)
{
    char full_message[512];
    int saved_errno = errno;

    if (snprintf(full_message,
                 sizeof(full_message),
                 "%s: %s",
                 message,
                 strerror(saved_errno)) >= (int)sizeof(full_message)) {
        mc_print_level(stderr, "err", message, MC_FMT_RED);
        return;
    }

    mc_print_level(stderr, "err", full_message, MC_FMT_RED);
}

void mc_print_section(const char *title)
{
    putchar('\n');
    if (mc_stream_supports_color(stdout)) {
        printf("%s%s%s\n", MC_FMT_BOLD_CYAN, title, MC_FMT_RESET);
        printf("%s------------------------------%s\n", MC_FMT_DIM, MC_FMT_RESET);
        fflush(stdout);
        return;
    }

    puts(title);
    puts("------------------------------");
    fflush(stdout);
}

void mc_print_subsection(const char *title)
{
    putchar('\n');
    if (mc_stream_supports_color(stdout)) {
        printf("%s%s%s\n", MC_FMT_BOLD_CYAN, title, MC_FMT_RESET);
        fflush(stdout);
        return;
    }

    puts(title);
    fflush(stdout);
}

void mc_print_kv_text(const char *label, const char *value)
{
    printf("  %-18s : %s\n", label, value);
    fflush(stdout);
}

void mc_print_kv_int(const char *label, long long value)
{
    printf("  %-18s : %lld\n", label, value);
    fflush(stdout);
}

void mc_print_kv_size(const char *label, size_t value)
{
    printf("  %-18s : %zu\n", label, value);
    fflush(stdout);
}

void mc_print_kv_u64(const char *label, unsigned long long value)
{
    printf("  %-18s : %llu\n", label, value);
    fflush(stdout);
}

void mc_print_kv_hex(const char *label, unsigned long long value)
{
    printf("  %-18s : 0x%llx\n", label, value);
    fflush(stdout);
}

void mc_print_prompt(void)
{
    if (mc_stream_supports_color(stdout)) {
        fputs(MC_FMT_BOLD_CYAN "mini-criu>" MC_FMT_RESET " ", stdout);
        fflush(stdout);
        return;
    }

    fputs("mini-criu> ", stdout);
    fflush(stdout);
}

typedef struct {
    char checkpoint_flag[MC_CHECKPOINT_FLAG_LEN];
    char checkpoint_code[MC_CHECKPOINT_CODE_LEN];
    char checkpoint_dir[PATH_MAX];
    char created_at[64];
    char status_snapshot[64];
    pid_t pid_target;
    bool has_checkpoint_flag;
} mc_checkpoint_catalog_entry;

static const char *mc_basename_from_path(const char *path)
{
    const char *last_separator = strrchr(path, '/');

    if (last_separator == NULL) {
        return path;
    }

    return last_separator + 1;
}

static int mc_copy_text_field(char *destination,
                              size_t destination_size,
                              const char *value,
                              const char *error_message)
{
    int written = snprintf(destination, destination_size, "%s", value);

    if (written < 0 || (size_t)written >= destination_size) {
        mc_log_error(error_message);
        return -1;
    }

    return 0;
}

static int mc_read_checkpoint_catalog_entry(const char *checkpoint_dir,
                                            mc_checkpoint_catalog_entry *entry)
{
    char checkpoint_info_path[PATH_MAX];
    FILE *file = NULL;
    char line[512];
    bool has_pid = false;
    bool has_code = false;

    memset(entry, 0, sizeof(*entry));
    entry->pid_target = -1;

    if (mc_join_path(checkpoint_info_path,
                     sizeof(checkpoint_info_path),
                     checkpoint_dir,
                     "checkpoint.info") != 0) {
        mc_log_error("Path checkpoint.info terlalu panjang.");
        return -1;
    }

    file = fopen(checkpoint_info_path, "r");
    if (file == NULL) {
        return -1;
    }

    if (mc_copy_text_field(entry->checkpoint_dir,
                           sizeof(entry->checkpoint_dir),
                           checkpoint_dir,
                           "Path direktori checkpoint terlalu panjang.") != 0) {
        fclose(file);
        return -1;
    }

    while (fgets(line, sizeof(line), file) != NULL) {
        char *equal_sign = NULL;
        char *key = NULL;
        char *value = NULL;

        mc_trim_newline(line);
        if (line[0] == '\0' || line[0] == '[') {
            continue;
        }

        equal_sign = strchr(line, '=');
        if (equal_sign == NULL) {
            continue;
        }

        *equal_sign = '\0';
        key = mc_trim_whitespace(line);
        value = mc_trim_whitespace(equal_sign + 1);

        if (strcmp(key, "checkpoint_flag") == 0) {
            if (mc_copy_text_field(entry->checkpoint_flag,
                                   sizeof(entry->checkpoint_flag),
                                   value,
                                   "Flag checkpoint terlalu panjang.") != 0) {
                fclose(file);
                return -1;
            }
            entry->has_checkpoint_flag = true;
        } else if (strcmp(key, "checkpoint_code") == 0) {
            if (mc_copy_text_field(entry->checkpoint_code,
                                   sizeof(entry->checkpoint_code),
                                   value,
                                   "Kode checkpoint terlalu panjang.") != 0) {
                fclose(file);
                return -1;
            }
            has_code = true;
        } else if (strcmp(key, "pid_target") == 0) {
            if (!mc_parse_pid(value, &entry->pid_target)) {
                fclose(file);
                mc_log_error("Nilai pid_target pada checkpoint.info tidak valid.");
                return -1;
            }
            has_pid = true;
        } else if (strcmp(key, "dibuat_pada") == 0) {
            if (mc_copy_text_field(entry->created_at,
                                   sizeof(entry->created_at),
                                   value,
                                   "Nilai dibuat_pada terlalu panjang.") != 0) {
                fclose(file);
                return -1;
            }
        } else if (strcmp(key, "status_snapshot") == 0) {
            if (mc_copy_text_field(entry->status_snapshot,
                                   sizeof(entry->status_snapshot),
                                   value,
                                   "Nilai status_snapshot terlalu panjang.") != 0) {
                fclose(file);
                return -1;
            }
        }
    }

    if (fclose(file) != 0) {
        mc_log_system_error("Gagal menutup checkpoint.info");
        return -1;
    }

    if (!has_code) {
        const char *base_name = mc_basename_from_path(checkpoint_dir);

        if (strncmp(base_name, "checkpoint-", 11) == 0) {
            base_name += 11;
        }

        if (mc_copy_text_field(entry->checkpoint_code,
                               sizeof(entry->checkpoint_code),
                               base_name,
                               "Kode checkpoint fallback terlalu panjang.") != 0) {
            return -1;
        }
    }

    if (!has_pid) {
        mc_log_error("checkpoint.info belum memuat pid_target yang dibutuhkan.");
        return -1;
    }

    if (entry->created_at[0] == '\0') {
        if (mc_copy_text_field(entry->created_at,
                               sizeof(entry->created_at),
                               "(tidak diketahui)",
                               "Nilai fallback dibuat_pada terlalu panjang.") != 0) {
            return -1;
        }
    }

    if (entry->status_snapshot[0] == '\0') {
        if (mc_copy_text_field(entry->status_snapshot,
                               sizeof(entry->status_snapshot),
                               "(tidak diketahui)",
                               "Nilai fallback status_snapshot terlalu panjang.") != 0) {
            return -1;
        }
    }

    return 0;
}

static int mc_append_checkpoint_catalog_entry(mc_checkpoint_catalog_entry **entries,
                                              size_t *count,
                                              size_t *capacity,
                                              const mc_checkpoint_catalog_entry *entry)
{
    mc_checkpoint_catalog_entry *new_entries = NULL;

    if (*count == *capacity) {
        size_t new_capacity = *capacity == 0 ? 8 : (*capacity * 2);

        new_entries = realloc(*entries, new_capacity * sizeof(*new_entries));
        if (new_entries == NULL) {
            mc_log_error("Gagal mengalokasikan memori untuk daftar checkpoint.");
            return -1;
        }

        *entries = new_entries;
        *capacity = new_capacity;
    }

    (*entries)[(*count)++] = *entry;
    return 0;
}

static bool mc_parse_checkpoint_flag_number(const char *flag, unsigned int *number_out)
{
    char *end = NULL;
    unsigned long value = 0;

    if (flag == NULL || flag[0] != 'F' || flag[1] == '\0') {
        return false;
    }

    errno = 0;
    value = strtoul(flag + 1, &end, 10);
    if (errno != 0 || end == (flag + 1) || *end != '\0' || value == 0 || value > 999999u) {
        return false;
    }

    *number_out = (unsigned int)value;
    return true;
}

static bool mc_is_checkpoint_flag_in_use(const mc_checkpoint_catalog_entry *entries,
                                         size_t count,
                                         const char *checkpoint_flag)
{
    for (size_t i = 0; i < count; ++i) {
        if (entries[i].checkpoint_flag[0] == '\0') {
            continue;
        }

        if (strcmp(entries[i].checkpoint_flag, checkpoint_flag) == 0) {
            return true;
        }
    }

    return false;
}

static int mc_append_checkpoint_flag_to_info(const char *checkpoint_dir, const char *checkpoint_flag)
{
    char checkpoint_info_path[PATH_MAX];
    FILE *file = NULL;

    if (mc_join_path(checkpoint_info_path,
                     sizeof(checkpoint_info_path),
                     checkpoint_dir,
                     "checkpoint.info") != 0) {
        mc_log_error("Path checkpoint.info terlalu panjang.");
        return -1;
    }

    file = fopen(checkpoint_info_path, "a");
    if (file == NULL) {
        mc_log_system_error("Gagal membuka checkpoint.info untuk menulis flag");
        return -1;
    }

    if (fprintf(file, "checkpoint_flag=%s\n", checkpoint_flag) < 0) {
        fclose(file);
        mc_log_system_error("Gagal menulis flag checkpoint");
        return -1;
    }

    if (fclose(file) != 0) {
        mc_log_system_error("Gagal menutup checkpoint.info");
        return -1;
    }

    return 0;
}

static int mc_assign_missing_checkpoint_flags(mc_checkpoint_catalog_entry *entries, size_t count)
{
    unsigned int next_number = 1;

    for (size_t i = 0; i < count; ++i) {
        unsigned int current_number = 0;

        if (!entries[i].has_checkpoint_flag) {
            continue;
        }

        if (!mc_parse_checkpoint_flag_number(entries[i].checkpoint_flag, &current_number)) {
            continue;
        }

        if (current_number >= next_number) {
            next_number = current_number + 1;
        }
    }

    for (size_t i = 0; i < count; ++i) {
        char checkpoint_flag[MC_CHECKPOINT_FLAG_LEN];

        if (entries[i].has_checkpoint_flag) {
            continue;
        }

        do {
            int written = snprintf(checkpoint_flag,
                                   sizeof(checkpoint_flag),
                                   "F%04u",
                                   next_number++);
            if (written < 0 || (size_t)written >= sizeof(checkpoint_flag)) {
                mc_log_error("Flag checkpoint terlalu panjang.");
                return -1;
            }
        } while (mc_is_checkpoint_flag_in_use(entries, count, checkpoint_flag));

        if (mc_append_checkpoint_flag_to_info(entries[i].checkpoint_dir, checkpoint_flag) != 0) {
            return -1;
        }

        if (mc_copy_text_field(entries[i].checkpoint_flag,
                               sizeof(entries[i].checkpoint_flag),
                               checkpoint_flag,
                               "Flag checkpoint terlalu panjang.") != 0) {
            return -1;
        }

        entries[i].has_checkpoint_flag = true;
    }

    return 0;
}

static int mc_collect_checkpoint_catalog(const mc_context *ctx,
                                         mc_checkpoint_catalog_entry **entries_out,
                                         size_t *count_out)
{
    DIR *directory = NULL;
    struct dirent *entry = NULL;
    mc_checkpoint_catalog_entry *entries = NULL;
    size_t count = 0;
    size_t capacity = 0;

    *entries_out = NULL;
    *count_out = 0;

    if (!mc_directory_exists(ctx->checkpoint_root)) {
        return 0;
    }

    directory = opendir(ctx->checkpoint_root);
    if (directory == NULL) {
        mc_log_system_error("Gagal membuka direktori root checkpoint");
        return -1;
    }

    while ((entry = readdir(directory)) != NULL) {
        char checkpoint_dir[PATH_MAX];
        char checkpoint_info_path[PATH_MAX];
        struct stat st;
        mc_checkpoint_catalog_entry catalog_entry;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        if (mc_join_path(checkpoint_dir, sizeof(checkpoint_dir), ctx->checkpoint_root, entry->d_name) != 0) {
            free(entries);
            closedir(directory);
            mc_log_error("Path direktori checkpoint terlalu panjang.");
            return -1;
        }

        if (stat(checkpoint_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
            continue;
        }

        if (mc_join_path(checkpoint_info_path,
                         sizeof(checkpoint_info_path),
                         checkpoint_dir,
                         "checkpoint.info") != 0) {
            free(entries);
            closedir(directory);
            mc_log_error("Path checkpoint.info terlalu panjang.");
            return -1;
        }

        if (stat(checkpoint_info_path, &st) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }

        if (mc_read_checkpoint_catalog_entry(checkpoint_dir, &catalog_entry) != 0) {
            free(entries);
            closedir(directory);
            return -1;
        }

        if (mc_append_checkpoint_catalog_entry(&entries, &count, &capacity, &catalog_entry) != 0) {
            free(entries);
            closedir(directory);
            return -1;
        }
    }

    if (closedir(directory) != 0) {
        free(entries);
        mc_log_system_error("Gagal menutup direktori root checkpoint");
        return -1;
    }

    if (mc_assign_missing_checkpoint_flags(entries, count) != 0) {
        free(entries);
        return -1;
    }

    *entries_out = entries;
    *count_out = count;
    return 0;
}

static int mc_find_checkpoint_catalog_entry(const mc_context *ctx,
                                            const char *reference,
                                            mc_checkpoint_catalog_entry *entry_out)
{
    mc_checkpoint_catalog_entry *entries = NULL;
    size_t count = 0;

    if (mc_collect_checkpoint_catalog(ctx, &entries, &count) != 0) {
        return -1;
    }

    for (size_t i = 0; i < count; ++i) {
        const char *base_name = mc_basename_from_path(entries[i].checkpoint_dir);

        if (strcmp(entries[i].checkpoint_flag, reference) != 0 &&
            strcmp(entries[i].checkpoint_code, reference) != 0 &&
            strcmp(base_name, reference) != 0 &&
            strcmp(entries[i].checkpoint_dir, reference) != 0) {
            continue;
        }

        *entry_out = entries[i];
        free(entries);
        return 0;
    }

    free(entries);
    mc_log_error("Flag checkpoint tidak ditemukan.");
    return -1;
}

static int mc_read_process_state(pid_t pid, char *state_out)
{
    char status_path[PATH_MAX];
    FILE *file = NULL;
    char line[256];

    if (snprintf(status_path, sizeof(status_path), "/proc/%d/status", pid) >= (int)sizeof(status_path)) {
        mc_log_error("Path status proses terlalu panjang.");
        return -1;
    }

    file = fopen(status_path, "r");
    if (file == NULL) {
        mc_log_system_error("Gagal membuka /proc/<pid>/status");
        return -1;
    }

    while (fgets(line, sizeof(line), file) != NULL) {
        if (strncmp(line, "State:", 6) != 0) {
            continue;
        }

        for (char *cursor = line + 6; *cursor != '\0'; ++cursor) {
            if (*cursor == ' ' || *cursor == '\t') {
                continue;
            }

            *state_out = *cursor;
            fclose(file);
            return 0;
        }
    }

    fclose(file);
    mc_log_error("State proses tidak ditemukan di /proc/<pid>/status.");
    return -1;
}

static int mc_append_resume_summary(const char *checkpoint_dir, const char *checkpoint_flag, pid_t pid)
{
    char checkpoint_info_path[PATH_MAX];
    char timestamp[32];
    FILE *file = NULL;

    if (mc_join_path(checkpoint_info_path,
                     sizeof(checkpoint_info_path),
                     checkpoint_dir,
                     "checkpoint.info") != 0) {
        mc_log_error("Path checkpoint.info terlalu panjang.");
        return -1;
    }

    mc_format_timestamp(timestamp, sizeof(timestamp));

    file = fopen(checkpoint_info_path, "a");
    if (file == NULL) {
        mc_log_system_error("Gagal membuka checkpoint.info untuk update resume");
        return -1;
    }

    if (fprintf(file,
                "\n"
                "checkpoint_flag=%s\n"
                "resume_manual=ya\n"
                "resume_manual_pada=%s\n"
                "resume_manual_pid=%d\n"
                "status_snapshot=dilanjutkan_kembali\n"
                "catatan_resume=Proses asli dilanjutkan kembali dengan command resume.\n",
                checkpoint_flag,
                timestamp,
                pid) < 0) {
        fclose(file);
        mc_log_system_error("Gagal menulis ringkasan resume");
        return -1;
    }

    if (fclose(file) != 0) {
        mc_log_system_error("Gagal menutup checkpoint.info");
        return -1;
    }

    return 0;
}

static int mc_compare_checkpoint_entries(const void *left, const void *right)
{
    const mc_checkpoint_catalog_entry *left_entry = left;
    const mc_checkpoint_catalog_entry *right_entry = right;
    int created_compare = strcmp(right_entry->created_at, left_entry->created_at);

    if (created_compare != 0) {
        return created_compare;
    }

    return strcmp(right_entry->checkpoint_code, left_entry->checkpoint_code);
}

/*
 * Melepaskan target yang masih ditahan oleh snapshot aktif.
 *
 * `detach_signal` menentukan sinyal apa yang dikirim saat `PTRACE_DETACH`.
 * Dengan nilai `0`, target diizinkan lanjut berjalan. Dengan `SIGSTOP`,
 * target tetap berada dalam keadaan stop sesudah tracer dilepas.
 */
int mc_release_snapshot(mc_context *ctx, bool announce, int detach_signal)
{
    if (!ctx->snapshot_active) {
        return 0;
    }

    if (ctx->target_pid > 0 &&
        ptrace(PTRACE_DETACH, ctx->target_pid, NULL, (void *)(long)detach_signal) == -1) {
        if (errno != ESRCH) {
            mc_log_system_error("Gagal melepaskan ptrace dari target");
            return -1;
        }

        if (announce) {
            mc_log_warn("Snapshot aktif dibersihkan, tetapi target sudah tidak tersedia.");
        }
    } else if (announce) {
        if (detach_signal == SIGSTOP) {
            mc_log_info("Target dilepas dari tracer dan tetap dalam keadaan stop.");
        } else {
            mc_log_info("Target dilepas kembali dan diizinkan berjalan.");
        }
    }

    ctx->snapshot_active = false;
    ctx->active_snapshot_id[0] = '\0';
    ctx->active_checkpoint_flag[0] = '\0';
    return 0;
}

int mc_list_checkpoints(const mc_context *ctx)
{
    mc_checkpoint_catalog_entry *entries = NULL;
    size_t count = 0;

    if (mc_collect_checkpoint_catalog(ctx, &entries, &count) != 0) {
        return 1;
    }

    mc_print_section("Daftar checkpoint");

    if (count == 0) {
        free(entries);
        mc_log_info("Belum ada checkpoint yang tersimpan.");
        return 0;
    }

    qsort(entries, count, sizeof(*entries), mc_compare_checkpoint_entries);

    printf("  %-8s %-8s %-26s %s\n",
           "Flag",
           "PID",
           "Dibuat",
           "Status");
    for (size_t i = 0; i < count; ++i) {
        printf("  %-8s %-8d %-26s %s\n",
               entries[i].checkpoint_flag,
               entries[i].pid_target,
               entries[i].created_at,
               entries[i].status_snapshot);
    }
    fflush(stdout);

    free(entries);
    return 0;
}

int mc_resume_checkpoint(mc_context *ctx, const char *reference)
{
    mc_checkpoint_catalog_entry entry;
    char checkpoint_path[PATH_MAX];
    char process_state = '\0';
    char message[160];

    if (mc_resolve_checkpoint_reference(ctx, reference, checkpoint_path, sizeof(checkpoint_path)) != 0) {
        return 1;
    }

    if (mc_find_checkpoint_catalog_entry(ctx, reference, &entry) != 0) {
        return 1;
    }

    if (strcmp(entry.status_snapshot, "aktif_menunggu_dump_memori") != 0) {
        mc_log_error("Checkpoint ini tidak sedang menahan proses freeze yang bisa di-resume.");
        return 1;
    }

    if (ctx->snapshot_active && strcmp(checkpoint_path, ctx->last_checkpoint_dir) == 0) {
        if (mc_append_resume_summary(checkpoint_path, entry.checkpoint_flag, entry.pid_target) != 0) {
            return 1;
        }

        if (mc_release_snapshot(ctx, true, 0) != 0) {
            return 1;
        }

        snprintf(message,
                 sizeof(message),
                 "Proses asli untuk checkpoint %s sudah dilanjutkan kembali.",
                 entry.checkpoint_flag);
        mc_log_ok(message);
        return 0;
    }

    if (!mc_process_exists(entry.pid_target)) {
        mc_log_error("PID target dari checkpoint ini sudah tidak berjalan.");
        return 1;
    }

    if (mc_read_process_state(entry.pid_target, &process_state) != 0) {
        return 1;
    }

    if (process_state != 'T' && process_state != 't') {
        mc_log_error("PID target dari checkpoint ini tidak sedang berada dalam keadaan stop.");
        return 1;
    }

    if (kill(entry.pid_target, SIGCONT) != 0) {
        mc_log_system_error("Gagal mengirim SIGCONT ke PID target");
        return 1;
    }

    if (mc_append_resume_summary(checkpoint_path, entry.checkpoint_flag, entry.pid_target) != 0) {
        return 1;
    }

    if (ctx->target_pid == entry.pid_target && ctx->snapshot_active) {
        ctx->snapshot_active = false;
        ctx->active_snapshot_id[0] = '\0';
        ctx->active_checkpoint_flag[0] = '\0';
    }

    snprintf(message,
             sizeof(message),
             "Proses PID %d untuk checkpoint %s sudah dilanjutkan kembali.",
             entry.pid_target,
             entry.checkpoint_flag);
    mc_log_ok(message);
    return 0;
}

int mc_allocate_checkpoint_flag(const mc_context *ctx,
                                char *checkpoint_flag,
                                size_t checkpoint_flag_size)
{
    mc_checkpoint_catalog_entry *entries = NULL;
    size_t count = 0;
    unsigned int next_number = 1;

    if (mc_collect_checkpoint_catalog(ctx, &entries, &count) != 0) {
        return -1;
    }

    for (size_t i = 0; i < count; ++i) {
        unsigned int current_number = 0;

        if (!mc_parse_checkpoint_flag_number(entries[i].checkpoint_flag, &current_number)) {
            continue;
        }

        if (current_number >= next_number) {
            next_number = current_number + 1;
        }
    }

    while (1) {
        int written = snprintf(checkpoint_flag, checkpoint_flag_size, "F%04u", next_number++);

        if (written < 0 || (size_t)written >= checkpoint_flag_size) {
            free(entries);
            mc_log_error("Flag checkpoint terlalu panjang.");
            return -1;
        }

        if (!mc_is_checkpoint_flag_in_use(entries, count, checkpoint_flag)) {
            break;
        }
    }

    free(entries);
    return 0;
}

int mc_resolve_checkpoint_reference(const mc_context *ctx,
                                    const char *reference,
                                    char *resolved_path,
                                    size_t resolved_path_size)
{
    mc_checkpoint_catalog_entry *entries = NULL;
    size_t count = 0;

    if (mc_directory_exists(reference)) {
        if (mc_copy_text_field(resolved_path,
                               resolved_path_size,
                               reference,
                               "Path checkpoint terlalu panjang.") != 0) {
            return -1;
        }
        return 0;
    }

    if (mc_collect_checkpoint_catalog(ctx, &entries, &count) != 0) {
        return -1;
    }

    for (size_t i = 0; i < count; ++i) {
        const char *base_name = mc_basename_from_path(entries[i].checkpoint_dir);

        if (strcmp(entries[i].checkpoint_flag, reference) != 0 &&
            strcmp(entries[i].checkpoint_code, reference) != 0 &&
            strcmp(base_name, reference) != 0) {
            continue;
        }

        if (mc_copy_text_field(resolved_path,
                               resolved_path_size,
                               entries[i].checkpoint_dir,
                               "Path checkpoint terlalu panjang.") != 0) {
            free(entries);
            return -1;
        }

        free(entries);
        return 0;
    }

    free(entries);
    mc_log_error("Flag checkpoint tidak ditemukan.");
    return -1;
}
