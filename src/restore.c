#include "mini_criu.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef struct {
    unsigned long long start_address;
    unsigned long long end_address;
    unsigned long long dump_offset;
    unsigned long long dumped_size;
    bool selected;
    bool dump_complete;
    bool has_dump_bytes;
} mc_restore_region;

typedef struct {
    char checkpoint_dir[PATH_MAX];
    char snapshot_id[MC_SNAPSHOT_ID_LEN];
    pid_t pid_target;
    size_t total_regions;
    size_t selected_regions;
    size_t dumped_regions;
    size_t skipped_regions;
    unsigned long long total_dumped_bytes;
    unsigned long long mem_dump_size;
    mc_restore_region *regions;
    size_t region_count;
} mc_restore_plan;

/*
 * Mengecek bahwa file checkpoint yang dibutuhkan benar-benar ada sebelum
 * metadata dibaca. Validasi ini dilakukan di awal agar proses persiapan
 * restore berhenti cepat jika checkpoint memang belum lengkap.
 */
static int mc_validate_checkpoint_files(const char *checkpoint_dir,
                                        char *checkpoint_info_path,
                                        size_t checkpoint_info_size,
                                        char *regs_path,
                                        size_t regs_size,
                                        char *mem_meta_path,
                                        size_t mem_meta_size,
                                        char *mem_dump_path,
                                        size_t mem_dump_size)
{
    struct stat st;

    if (mc_join_path(checkpoint_info_path, checkpoint_info_size, checkpoint_dir, "checkpoint.info") != 0 ||
        mc_join_path(regs_path, regs_size, checkpoint_dir, "regs.dump") != 0 ||
        mc_join_path(mem_meta_path, mem_meta_size, checkpoint_dir, "mem.meta") != 0 ||
        mc_join_path(mem_dump_path, mem_dump_size, checkpoint_dir, "mem.dump") != 0) {
        mc_log_error("Path file checkpoint terlalu panjang.");
        return -1;
    }

    if (stat(checkpoint_info_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        mc_log_error("File checkpoint.info tidak ditemukan.");
        return -1;
    }

    if (stat(regs_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        mc_log_error("File regs.dump tidak ditemukan.");
        return -1;
    }

    if (stat(mem_meta_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        mc_log_error("File mem.meta tidak ditemukan.");
        return -1;
    }

    if (stat(mem_dump_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        mc_log_error("File mem.dump tidak ditemukan.");
        return -1;
    }

    return 0;
}

/*
 * Memecah satu baris `kunci=nilai` dari file metadata checkpoint.
 *
 * File metadata proyek ini sengaja berbentuk teks biasa agar mudah diperiksa
 * secara manual. Helper ini membuat parser tetap sederhana.
 */
static int mc_split_metadata_line(char *line,
                                  char *key,
                                  size_t key_size,
                                  char *value,
                                  size_t value_size)
{
    char *equal_sign = NULL;
    char *trimmed_key = NULL;
    char *trimmed_value = NULL;

    mc_trim_newline(line);
    if (line[0] == '\0' || line[0] == '[') {
        return 1;
    }

    equal_sign = strchr(line, '=');
    if (equal_sign == NULL) {
        return 1;
    }

    *equal_sign = '\0';
    trimmed_key = mc_trim_whitespace(line);
    trimmed_value = mc_trim_whitespace(equal_sign + 1);

    if (snprintf(key, key_size, "%s", trimmed_key) >= (int)key_size ||
        snprintf(value, value_size, "%s", trimmed_value) >= (int)value_size) {
        mc_log_error("Baris metadata terlalu panjang untuk diproses.");
        return -1;
    }

    return 0;
}

/*
 * Mengubah angka desimal di metadata menjadi `size_t`.
 */
static int mc_parse_size_value(const char *text, size_t *value_out)
{
    char *end = NULL;
    unsigned long long value = 0;

    errno = 0;
    value = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') {
        return -1;
    }

    *value_out = (size_t)value;
    return 0;
}

/*
 * Mengubah angka heksadesimal atau desimal di metadata menjadi `unsigned long long`.
 */
static int mc_parse_ull_value(const char *text, unsigned long long *value_out)
{
    char *end = NULL;
    int base = 10;

    if (strncmp(text, "0x", 2) == 0 || strncmp(text, "0X", 2) == 0) {
        base = 16;
    }

    errno = 0;
    *value_out = strtoull(text, &end, base);
    if (errno != 0 || end == text || *end != '\0') {
        return -1;
    }

    return 0;
}

/*
 * Menyalin snapshot ID ke struktur internal dengan validasi panjang.
 */
static int mc_copy_snapshot_id(char *destination, size_t destination_size, const char *source)
{
    if (strlen(source) >= destination_size) {
        mc_log_error("Snapshot ID pada metadata terlalu panjang.");
        return -1;
    }

    snprintf(destination, destination_size, "%s", source);
    return 0;
}

/*
 * Menambah satu entri region ke rencana restore internal.
 *
 * Struktur ini dipakai agar `restore` tidak hanya mencetak file mentah, tetapi
 * benar-benar memahami region mana yang nantinya perlu dipetakan kembali.
 */
static int mc_append_restore_region(mc_restore_plan *plan, const mc_restore_region *region)
{
    mc_restore_region *new_regions = NULL;

    new_regions = realloc(plan->regions, (plan->region_count + 1) * sizeof(*plan->regions));
    if (new_regions == NULL) {
        mc_log_error("Gagal mengalokasikan memori untuk rencana restore.");
        return -1;
    }

    plan->regions = new_regions;
    plan->regions[plan->region_count++] = *region;
    return 0;
}

/*
 * `checkpoint.info` menyumbang identitas snapshot dan ringkasan tinggi
 * checkpoint, termasuk PID target serta status umum dump memori.
 */
static int mc_load_checkpoint_info(const char *path, mc_restore_plan *plan)
{
    FILE *file = NULL;
    char line[512];
    bool has_snapshot_id = false;
    bool has_pid = false;
    bool has_memory_snapshot_id = false;

    file = fopen(path, "r");
    if (file == NULL) {
        mc_log_system_error("Gagal membuka checkpoint.info");
        return -1;
    }

    while (fgets(line, sizeof(line), file) != NULL) {
        char key[128];
        char value[384];
        int split_status = mc_split_metadata_line(line, key, sizeof(key), value, sizeof(value));

        if (split_status > 0) {
            continue;
        }
        if (split_status < 0) {
            fclose(file);
            return -1;
        }

        if (strcmp(key, "snapshot_id") == 0) {
            if (mc_copy_snapshot_id(plan->snapshot_id, sizeof(plan->snapshot_id), value) != 0) {
                fclose(file);
                return -1;
            }
            has_snapshot_id = true;
        } else if (strcmp(key, "pid_target") == 0) {
            if (!mc_parse_pid(value, &plan->pid_target)) {
                fclose(file);
                mc_log_error("Nilai pid_target pada checkpoint.info tidak valid.");
                return -1;
            }
            has_pid = true;
        } else if (strcmp(key, "snapshot_id_memori") == 0) {
            if (has_snapshot_id && strcmp(plan->snapshot_id, value) != 0) {
                fclose(file);
                mc_log_error("Snapshot ID pada ringkasan memori tidak cocok dengan metadata freeze.");
                return -1;
            }
            has_memory_snapshot_id = true;
        } else if (strcmp(key, "pid_target_memori") == 0) {
            pid_t mem_pid = -1;

            if (!mc_parse_pid(value, &mem_pid)) {
                fclose(file);
                mc_log_error("Nilai pid_target_memori pada checkpoint.info tidak valid.");
                return -1;
            }

            if (has_pid && plan->pid_target != mem_pid) {
                fclose(file);
                mc_log_error("PID target pada ringkasan memori tidak cocok dengan metadata freeze.");
                return -1;
            }
        }
    }

    if (fclose(file) != 0) {
        mc_log_system_error("Gagal menutup checkpoint.info");
        return -1;
    }

    if (!has_snapshot_id || !has_pid || !has_memory_snapshot_id) {
        mc_log_error("checkpoint.info belum memuat metadata snapshot yang dibutuhkan.");
        return -1;
    }

    return 0;
}

/*
 * `mem.meta` menyumbang detail yang lebih dekat ke restore:
 * jumlah region, region terpilih, offset dump, ukuran dump, dan status tiap
 * region di dalam `mem.dump`.
 */
static int mc_load_memory_metadata(const char *path, mc_restore_plan *plan)
{
    FILE *file = NULL;
    char line[512];
    bool in_region = false;
    bool has_snapshot_id = false;
    bool has_pid = false;
    bool has_total_regions = false;
    bool has_selected_regions = false;
    bool has_dumped_regions = false;
    bool has_total_bytes = false;
    mc_restore_region current_region;
    size_t parsed_selected_regions = 0;
    size_t parsed_dumped_regions = 0;
    unsigned long long parsed_dumped_bytes = 0;
    unsigned long long expected_offset = 0;

    memset(&current_region, 0, sizeof(current_region));

    file = fopen(path, "r");
    if (file == NULL) {
        mc_log_system_error("Gagal membuka mem.meta");
        return -1;
    }

    while (fgets(line, sizeof(line), file) != NULL) {
        if (strncmp(line, "[region_", 8) == 0) {
            if (in_region) {
                if (mc_append_restore_region(plan, &current_region) != 0) {
                    fclose(file);
                    return -1;
                }
            }

            memset(&current_region, 0, sizeof(current_region));
            in_region = true;
            continue;
        }

        {
            char key[128];
            char value[384];
            int split_status = mc_split_metadata_line(line, key, sizeof(key), value, sizeof(value));

            if (split_status > 0) {
                continue;
            }
            if (split_status < 0) {
                fclose(file);
                return -1;
            }

            if (!in_region) {
                if (strcmp(key, "snapshot_id") == 0) {
                    if (plan->snapshot_id[0] != '\0' && strcmp(plan->snapshot_id, value) != 0) {
                        fclose(file);
                        mc_log_error("Snapshot ID pada mem.meta tidak cocok dengan checkpoint.info.");
                        return -1;
                    }
                    if (mc_copy_snapshot_id(plan->snapshot_id, sizeof(plan->snapshot_id), value) != 0) {
                        fclose(file);
                        return -1;
                    }
                    has_snapshot_id = true;
                } else if (strcmp(key, "pid_target") == 0) {
                    pid_t meta_pid = -1;

                    if (!mc_parse_pid(value, &meta_pid)) {
                        fclose(file);
                        mc_log_error("Nilai pid_target pada mem.meta tidak valid.");
                        return -1;
                    }

                    if (plan->pid_target > 0 && plan->pid_target != meta_pid) {
                        fclose(file);
                        mc_log_error("PID target pada mem.meta tidak cocok dengan checkpoint.info.");
                        return -1;
                    }

                    plan->pid_target = meta_pid;
                    has_pid = true;
                } else if (strcmp(key, "jumlah_region") == 0) {
                    if (mc_parse_size_value(value, &plan->total_regions) != 0) {
                        fclose(file);
                        mc_log_error("Nilai jumlah_region pada mem.meta tidak valid.");
                        return -1;
                    }
                    has_total_regions = true;
                } else if (strcmp(key, "jumlah_region_terpilih") == 0) {
                    if (mc_parse_size_value(value, &plan->selected_regions) != 0) {
                        fclose(file);
                        mc_log_error("Nilai jumlah_region_terpilih pada mem.meta tidak valid.");
                        return -1;
                    }
                    has_selected_regions = true;
                } else if (strcmp(key, "jumlah_region_dump_berhasil") == 0) {
                    if (mc_parse_size_value(value, &plan->dumped_regions) != 0) {
                        fclose(file);
                        mc_log_error("Nilai jumlah_region_dump_berhasil pada mem.meta tidak valid.");
                        return -1;
                    }
                    has_dumped_regions = true;
                } else if (strcmp(key, "jumlah_region_dump_terlewati") == 0) {
                    if (mc_parse_size_value(value, &plan->skipped_regions) != 0) {
                        fclose(file);
                        mc_log_error("Nilai jumlah_region_dump_terlewati pada mem.meta tidak valid.");
                        return -1;
                    }
                } else if (strcmp(key, "jumlah_byte_dump") == 0) {
                    if (mc_parse_ull_value(value, &plan->total_dumped_bytes) != 0) {
                        fclose(file);
                        mc_log_error("Nilai jumlah_byte_dump pada mem.meta tidak valid.");
                        return -1;
                    }
                    has_total_bytes = true;
                }
            } else {
                if (strcmp(key, "dipilih") == 0) {
                    current_region.selected = strcmp(value, "ya") == 0;
                } else if (strcmp(key, "mulai") == 0) {
                    if (mc_parse_ull_value(value, &current_region.start_address) != 0) {
                        fclose(file);
                        mc_log_error("Nilai alamat awal region pada mem.meta tidak valid.");
                        return -1;
                    }
                } else if (strcmp(key, "akhir") == 0) {
                    if (mc_parse_ull_value(value, &current_region.end_address) != 0) {
                        fclose(file);
                        mc_log_error("Nilai alamat akhir region pada mem.meta tidak valid.");
                        return -1;
                    }
                } else if (strcmp(key, "offset_dump") == 0) {
                    if (mc_parse_ull_value(value, &current_region.dump_offset) != 0) {
                        fclose(file);
                        mc_log_error("Nilai offset_dump pada mem.meta tidak valid.");
                        return -1;
                    }
                } else if (strcmp(key, "ukuran_dump") == 0) {
                    if (mc_parse_ull_value(value, &current_region.dumped_size) != 0) {
                        fclose(file);
                        mc_log_error("Nilai ukuran_dump pada mem.meta tidak valid.");
                        return -1;
                    }
                } else if (strcmp(key, "status_dump") == 0) {
                    current_region.dump_complete = strcmp(value, "berhasil") == 0;
                    current_region.has_dump_bytes = current_region.dump_complete ||
                                                    strcmp(value, "parsial") == 0;
                }
            }
        }
    }

    if (in_region) {
        if (mc_append_restore_region(plan, &current_region) != 0) {
            fclose(file);
            return -1;
        }
    }

    if (fclose(file) != 0) {
        mc_log_system_error("Gagal menutup mem.meta");
        return -1;
    }

    if (!has_snapshot_id || !has_pid || !has_total_regions || !has_selected_regions ||
        !has_dumped_regions || !has_total_bytes) {
        mc_log_error("mem.meta belum memuat metadata yang dibutuhkan untuk persiapan restore.");
        return -1;
    }

    for (size_t i = 0; i < plan->region_count; ++i) {
        if (plan->regions[i].selected) {
            ++parsed_selected_regions;
        }

        if (plan->regions[i].dump_complete) {
            ++parsed_dumped_regions;
        }

        if (plan->regions[i].has_dump_bytes) {
            if (plan->regions[i].dump_offset != expected_offset) {
                mc_log_error("Offset dump region pada mem.meta tidak berurutan.");
                return -1;
            }

            parsed_dumped_bytes += plan->regions[i].dumped_size;
            expected_offset += plan->regions[i].dumped_size;
        }
    }

    if (plan->region_count != plan->total_regions) {
        mc_log_error("Jumlah entri region di mem.meta tidak cocok dengan header.");
        return -1;
    }

    if (parsed_selected_regions != plan->selected_regions) {
        mc_log_error("Jumlah region terpilih di mem.meta tidak cocok dengan header.");
        return -1;
    }

    if (parsed_dumped_regions != plan->dumped_regions) {
        mc_log_error("Jumlah region dump di mem.meta tidak cocok dengan header.");
        return -1;
    }

    if (parsed_dumped_bytes != plan->total_dumped_bytes) {
        mc_log_error("Jumlah byte dump di mem.meta tidak cocok dengan isi region.");
        return -1;
    }

    return 0;
}

/*
 * Ukuran `mem.dump` dibandingkan dengan metadata agar rencana restore tidak
 * dibangun dari checkpoint yang terpotong atau tidak lengkap.
 */
static int mc_validate_dump_size(const char *mem_dump_path, mc_restore_plan *plan)
{
    struct stat st;

    if (stat(mem_dump_path, &st) != 0) {
        mc_log_system_error("Gagal membaca ukuran file mem.dump");
        return -1;
    }

    plan->mem_dump_size = (unsigned long long)st.st_size;
    if (plan->mem_dump_size != plan->total_dumped_bytes) {
        mc_log_error("Ukuran file mem.dump tidak cocok dengan metadata checkpoint.");
        return -1;
    }

    return 0;
}

/*
 * Ringkasan ini menunjukkan bahwa checkpoint berhasil dimuat dan sudah cukup
 * lengkap untuk masuk ke tahap restore berikutnya, walaupun eksekusi restore
 * nyata belum dilakukan.
 */
static void mc_print_restore_plan_summary(const mc_restore_plan *plan)
{
    puts("");
    puts("ringkasan persiapan restore");
    puts("---------------------------");
    printf("Checkpoint dimuat : %s\n", plan->checkpoint_dir);
    printf("Snapshot ID       : %s\n", plan->snapshot_id);
    printf("PID target        : %d\n", plan->pid_target);
    printf("Jumlah region     : %zu\n", plan->total_regions);
    printf("Region terpilih   : %zu\n", plan->selected_regions);
    printf("Region didump     : %zu\n", plan->dumped_regions);
    printf("Region terlewati  : %zu\n", plan->skipped_regions);
    printf("Total byte dump   : %llu\n", plan->total_dumped_bytes);
    printf("Ukuran mem.dump   : %llu\n", plan->mem_dump_size);
    puts("Status restore    : metadata checkpoint valid dan rencana restore awal berhasil disusun.");
    puts("Catatan           : eksekusi restore belum dijalankan.");
    puts("");
}

int mc_restore_checkpoint(mc_context *ctx, const char *checkpoint_dir)
{
    char checkpoint_info_path[PATH_MAX];
    char regs_path[PATH_MAX];
    char mem_meta_path[PATH_MAX];
    char mem_dump_path[PATH_MAX];
    mc_restore_plan plan;

    memset(&plan, 0, sizeof(plan));

    if (ctx->snapshot_active) {
        mc_log_error("Masih ada snapshot aktif. Selesaikan dulu dengan 'dump-memory' atau keluar dari CLI.");
        return 1;
    }

    /*
     * Restore preparation hanya boleh dimulai dari direktori checkpoint yang
     * benar-benar ada di disk.
     */
    if (!mc_directory_exists(checkpoint_dir)) {
        mc_log_error("Direktori checkpoint tidak ada.");
        return 1;
    }

    if (snprintf(plan.checkpoint_dir, sizeof(plan.checkpoint_dir), "%s", checkpoint_dir) >=
        (int)sizeof(plan.checkpoint_dir)) {
        mc_log_error("Path checkpoint terlalu panjang.");
        return 1;
    }

    if (mc_validate_checkpoint_files(checkpoint_dir,
                                     checkpoint_info_path,
                                     sizeof(checkpoint_info_path),
                                     regs_path,
                                     sizeof(regs_path),
                                     mem_meta_path,
                                     sizeof(mem_meta_path),
                                     mem_dump_path,
                                     sizeof(mem_dump_path)) != 0) {
        free(plan.regions);
        return 1;
    }

    /*
     * Metadata dibaca lebih dulu agar tool memahami identitas snapshot dan
     * susunan byte di `mem.dump` sebelum ada usaha restore yang lebih jauh.
     */
    if (mc_load_checkpoint_info(checkpoint_info_path, &plan) != 0) {
        free(plan.regions);
        return 1;
    }

    if (mc_load_memory_metadata(mem_meta_path, &plan) != 0) {
        free(plan.regions);
        return 1;
    }

    if (mc_validate_dump_size(mem_dump_path, &plan) != 0) {
        free(plan.regions);
        return 1;
    }

    snprintf(ctx->last_checkpoint_dir, sizeof(ctx->last_checkpoint_dir), "%s", checkpoint_dir);

    puts("Checkpoint berhasil dimuat untuk persiapan restore.");
    mc_print_restore_plan_summary(&plan);

    /*
     * Persiapan restore berhenti di sini. Struktur internal sudah cukup untuk
     * tahap berikutnya, tetapi tool belum membuat proses baru atau menulis
     * register/memori ke proses manapun.
     */
    free(plan.regions);
    return 0;
}
