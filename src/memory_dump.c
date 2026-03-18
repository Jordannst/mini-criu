#include "mini_criu.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Menyiapkan direktori checkpoint untuk fase metadata memori.
 *
 * Jika sudah ada checkpoint terakhir dari fase freeze, direktori itu digunakan
 * kembali agar file `regs.dump`, `checkpoint.info`, dan `mem.meta` berada pada
 * folder yang sama. Jika belum ada, fungsi ini membuat direktori baru.
 */
static int mc_prepare_memory_checkpoint_dir(mc_context *ctx,
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
 * /proc/<pid>/maps berisi daftar region memori virtual milik proses.
 *
 * Pada fase ini kita hanya membaca metadata region, bukan isi memorinya.
 * Metadata ini akan dipakai sebagai dasar untuk fase dump memori berikutnya.
 */
static int mc_parse_maps_line(const char *line, mc_memory_region *region)
{
    unsigned long long start = 0;
    unsigned long long end = 0;
    unsigned long long offset = 0;
    char permissions[MC_REGION_PERMS_LEN] = {0};
    char device[32] = {0};
    unsigned long inode = 0;
    char label[MC_REGION_LABEL_LEN] = {0};
    const char *label_start = label;
    int matched = 0;

    matched = sscanf(line,
                     "%llx-%llx %4s %llx %31s %lu %255[^\n]",
                     &start,
                     &end,
                     permissions,
                     &offset,
                     device,
                     &inode,
                     label);

    if (matched < 6) {
        return -1;
    }

    region->start_address = start;
    region->end_address = end;
    region->offset = offset;
    snprintf(region->permissions, sizeof(region->permissions), "%s", permissions);

    if (matched == 7) {
        while (*label_start != '\0' && isspace((unsigned char)*label_start)) {
            ++label_start;
        }
        snprintf(region->label, sizeof(region->label), "%s", label_start);
    } else {
        region->label[0] = '\0';
    }

    region->selected = false;
    return 0;
}

/*
 * Aturan seleksi Phase 3 sengaja dibuat sederhana:
 * - pilih hanya region writable private (`rw-p`)
 *
 * Aturan ini biasanya sudah mencakup heap, stack, dan banyak region anonim
 * privat yang nanti paling relevan untuk fase dump memori.
 */
static bool mc_should_select_region(const mc_memory_region *region)
{
    return region->permissions[1] == 'w' && region->permissions[3] == 'p';
}

/*
 * Membaca seluruh `/proc/<pid>/maps` dan menyimpannya ke array dinamis.
 *
 * Array dinamis dipakai agar struktur data tetap sederhana untuk dipahami,
 * sambil tetap bisa menangani jumlah region yang tidak diketahui sejak awal.
 */
static int mc_load_memory_regions(pid_t pid, mc_memory_region **regions_out, size_t *count_out)
{
    char maps_path[PATH_MAX];
    FILE *maps_file = NULL;
    mc_memory_region *regions = NULL;
    size_t count = 0;
    size_t capacity = 0;
    char line[512];

    if (snprintf(maps_path, sizeof(maps_path), "/proc/%d/maps", pid) >= (int)sizeof(maps_path)) {
        mc_log_error("Path file maps terlalu panjang.");
        return -1;
    }

    maps_file = fopen(maps_path, "r");
    if (maps_file == NULL) {
        mc_log_system_error("Gagal membuka /proc/<pid>/maps");
        return -1;
    }

    while (fgets(line, sizeof(line), maps_file) != NULL) {
        mc_memory_region region;
        mc_memory_region *new_regions;

        if (mc_parse_maps_line(line, &region) != 0) {
            continue;
        }

        region.selected = mc_should_select_region(&region);

        if (count == capacity) {
            size_t new_capacity = capacity == 0 ? 16 : capacity * 2;

            new_regions = realloc(regions, new_capacity * sizeof(*regions));
            if (new_regions == NULL) {
                fclose(maps_file);
                free(regions);
                mc_log_error("Gagal mengalokasikan memori untuk metadata region.");
                return -1;
            }

            regions = new_regions;
            capacity = new_capacity;
        }

        regions[count++] = region;
    }

    if (fclose(maps_file) != 0) {
        free(regions);
        mc_log_system_error("Gagal menutup file /proc/<pid>/maps");
        return -1;
    }

    *regions_out = regions;
    *count_out = count;
    return 0;
}

/*
 * File `mem.meta` menyimpan metadata region memori dalam format teks yang mudah
 * dibaca. Tujuannya agar hasil parsing dapat diperiksa langsung sebelum fase
 * dump byte mentah diimplementasikan.
 */
static int mc_write_memory_metadata_file(const char *path,
                                         pid_t pid,
                                         const char *timestamp,
                                         const mc_memory_region *regions,
                                         size_t count,
                                         size_t selected_count)
{
    FILE *file = fopen(path, "w");

    if (file == NULL) {
        mc_log_system_error("Gagal membuka file metadata memori");
        return -1;
    }

    if (fprintf(file,
                "fase=metadata_memori\n"
                "pid_target=%d\n"
                "dibuat_pada=%s\n"
                "aturan_seleksi=region_writable_private_rw-p\n"
                "jumlah_region=%zu\n"
                "jumlah_region_terpilih=%zu\n\n",
                pid,
                timestamp,
                count,
                selected_count) < 0) {
        fclose(file);
        mc_log_system_error("Gagal menulis header metadata memori");
        return -1;
    }

    for (size_t i = 0; i < count; ++i) {
        unsigned long long region_size = regions[i].end_address - regions[i].start_address;
        const char *label = regions[i].label[0] != '\0' ? regions[i].label : "(anonim)";

        if (fprintf(file,
                    "[region_%zu]\n"
                    "dipilih=%s\n"
                    "mulai=0x%llx\n"
                    "akhir=0x%llx\n"
                    "ukuran_byte=%llu\n"
                    "izin=%s\n"
                    "offset=0x%llx\n"
                    "label=%s\n\n",
                    i,
                    regions[i].selected ? "ya" : "tidak",
                    regions[i].start_address,
                    regions[i].end_address,
                    region_size,
                    regions[i].permissions,
                    regions[i].offset,
                    label) < 0) {
            fclose(file);
            mc_log_system_error("Gagal menulis detail region memori");
            return -1;
        }
    }

    if (fclose(file) != 0) {
        mc_log_system_error("Gagal menutup file metadata memori");
        return -1;
    }

    return 0;
}

/*
 * Ringkasan ini ditambahkan ke `checkpoint.info` agar direktori checkpoint
 * langsung menunjukkan bahwa peta memori sudah diparsing pada Phase 3.
 */
static int mc_append_memory_summary(const char *path,
                                    pid_t pid,
                                    const char *timestamp,
                                    size_t total_regions,
                                    size_t selected_regions)
{
    FILE *file = fopen(path, "a");

    if (file == NULL) {
        mc_log_system_error("Gagal membuka checkpoint.info untuk diringkas");
        return -1;
    }

    if (fprintf(file,
                "\n"
                "fase_memori=metadata_maps\n"
                "pid_target_memori=%d\n"
                "dibuat_pada_memori=%s\n"
                "file_peta_memori=mem.meta\n"
                "jumlah_region=%zu\n"
                "jumlah_region_terpilih=%zu\n"
                "catatan_memori=Fase ini hanya menyimpan metadata region tanpa membaca /proc/<pid>/mem.\n",
                pid,
                timestamp,
                total_regions,
                selected_regions) < 0) {
        fclose(file);
        mc_log_system_error("Gagal menambahkan ringkasan metadata memori");
        return -1;
    }

    if (fclose(file) != 0) {
        mc_log_system_error("Gagal menutup checkpoint.info");
        return -1;
    }

    return 0;
}

int mc_dump_memory(mc_context *ctx)
{
    char timestamp[32];
    char checkpoint_dir[PATH_MAX];
    char metadata_path[PATH_MAX];
    char mem_meta_path[PATH_MAX];
    mc_memory_region *regions = NULL;
    size_t region_count = 0;
    size_t selected_count = 0;
    int result = 1;

    if (ctx->target_pid <= 0) {
        mc_log_error("Belum ada target yang dipilih. Gunakan 'set-target <pid>' terlebih dahulu.");
        return 1;
    }

    if (!mc_process_exists(ctx->target_pid)) {
        mc_log_error("PID target yang dipilih sedang tidak berjalan.");
        return 1;
    }

    mc_format_timestamp(timestamp, sizeof(timestamp));

    if (mc_load_memory_regions(ctx->target_pid, &regions, &region_count) != 0) {
        return 1;
    }

    if (region_count == 0) {
        free(regions);
        mc_log_error("Tidak ada region memori yang berhasil diparsing dari /proc/<pid>/maps.");
        return 1;
    }

    for (size_t i = 0; i < region_count; ++i) {
        if (regions[i].selected) {
            ++selected_count;
        }
    }

    if (mc_prepare_memory_checkpoint_dir(ctx, timestamp, checkpoint_dir, sizeof(checkpoint_dir)) != 0) {
        goto cleanup;
    }

    if (mc_join_path(metadata_path, sizeof(metadata_path), checkpoint_dir, "checkpoint.info") != 0) {
        mc_log_error("Path metadata terlalu panjang.");
        goto cleanup;
    }

    if (mc_join_path(mem_meta_path, sizeof(mem_meta_path), checkpoint_dir, "mem.meta") != 0) {
        mc_log_error("Path file metadata memori terlalu panjang.");
        goto cleanup;
    }

    if (mc_write_memory_metadata_file(mem_meta_path,
                                      ctx->target_pid,
                                      timestamp,
                                      regions,
                                      region_count,
                                      selected_count) != 0) {
        goto cleanup;
    }

    if (mc_append_memory_summary(metadata_path,
                                 ctx->target_pid,
                                 timestamp,
                                 region_count,
                                 selected_count) != 0) {
        goto cleanup;
    }

    printf("Metadata peta memori berhasil disimpan di: %s\n", checkpoint_dir);
    printf("Jumlah region yang diparsing: %zu\n", region_count);
    printf("Jumlah region yang dipilih untuk fase berikutnya: %zu\n", selected_count);
    puts("File yang dihasilkan: mem.meta.");
    puts("TODO: fase berikutnya akan membaca byte mentah dari region yang sudah dipilih.");
    result = 0;

cleanup:
    free(regions);
    return result;
}
