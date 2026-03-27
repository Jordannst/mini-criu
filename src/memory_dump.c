#include "mini_criu.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <unistd.h>

/*
 * Menyiapkan direktori checkpoint untuk hasil dump memori.
 *
 * Dump memori hanya boleh melanjutkan snapshot yang sudah dibuka oleh `freeze`.
 * Karena itu, fungsi ini hanya memakai direktori checkpoint yang sudah
 * tercatat di konteks dan tidak membuat snapshot baru sendiri.
 */
static int mc_prepare_memory_checkpoint_dir(mc_context *ctx,
                                            const char *timestamp,
                                            char *checkpoint_dir,
                                            size_t size)
{
    int written;
    (void)timestamp;

    if (ctx->last_checkpoint_dir[0] == '\0' || !mc_directory_exists(ctx->last_checkpoint_dir)) {
        mc_log_error("Direktori snapshot aktif tidak tersedia.");
        return -1;
    }

    written = snprintf(checkpoint_dir, size, "%s", ctx->last_checkpoint_dir);
    if (written < 0 || (size_t)written >= size) {
        mc_log_error("Path checkpoint terakhir terlalu panjang.");
        return -1;
    }

    return 0;
}

/*
 * /proc/<pid>/maps berisi daftar region memori virtual milik proses.
 *
 * Fungsi ini memecah satu baris maps menjadi struktur `mc_memory_region`
 * sehingga alamat, izin akses, offset, dan label region bisa dipakai kembali
 * saat metadata ditulis dan dump memori dilakukan.
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
    region->dump_offset = 0;
    region->dumped_size = 0;
    snprintf(region->permissions, sizeof(region->permissions), "%s", permissions);
    snprintf(region->dump_status, sizeof(region->dump_status), "%s", "belum_diproses");

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
 * Menentukan region mana yang layak dibaca dari `/proc/<pid>/mem`.
 *
 * Aturan yang dipakai sengaja sederhana: pilih hanya region writable private
 * (`rw-p`) karena region seperti ini biasanya memuat data proses yang berubah
 * saat program berjalan, misalnya heap, stack, dan region anonim writable.
 */
static bool mc_should_select_region(const mc_memory_region *region)
{
    return region->permissions[1] == 'w' && region->permissions[3] == 'p';
}

/*
 * Membaca seluruh `/proc/<pid>/maps` saat target sedang berhenti.
 *
 * Target dihentikan sementara sebelum parsing dilakukan agar metadata region dan
 * raw byte yang nanti dibaca berasal dari kondisi proses yang sama selama
 * command `dump-memory` berlangsung.
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

    /*
     * Setiap baris maps diparsing menjadi satu entri region. Hasilnya disimpan
     * ke array dinamis karena jumlah region berbeda untuk tiap proses.
     */
    while (fgets(line, sizeof(line), maps_file) != NULL) {
        mc_memory_region region;
        mc_memory_region *new_regions;

        if (mc_parse_maps_line(line, &region) != 0) {
            continue;
        }

        region.selected = mc_should_select_region(&region);
        if (!region.selected) {
            snprintf(region.dump_status, sizeof(region.dump_status), "%s", "tidak_dipilih");
        }

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
 * Menulis seluruh buffer ke file dump.
 *
 * Fungsi ini menangani write parsial supaya offset di `mem.dump` tetap
 * berurutan dan ukuran yang tercatat benar-benar cocok dengan byte yang
 * berhasil ditulis ke disk.
 */
static int mc_write_all(int fd, const unsigned char *buffer, size_t size)
{
    size_t total_written = 0;

    while (total_written < size) {
        ssize_t written = write(fd, buffer + total_written, size - total_written);

        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }

        total_written += (size_t)written;
    }

    return 0;
}

/*
 * /proc/<pid>/mem memberi akses ke ruang alamat virtual proses target.
 *
 * `mem.dump` memakai format paling sederhana:
 * - byte mentah ditulis berurutan
 * - urutan region mengikuti entri yang ditandai `dipilih=ya` di `mem.meta`
 * - `dump_offset` dan `dumped_size` di `mem.meta` menjelaskan cara membaca
 *   kembali potongan byte untuk setiap region
 */
static int mc_dump_selected_regions(pid_t pid,
                                    const char *mem_dump_path,
                                    mc_memory_region *regions,
                                    size_t region_count,
                                    size_t *dumped_regions_out,
                                    size_t *skipped_regions_out,
                                    unsigned long long *total_dumped_bytes_out)
{
    char mem_path[PATH_MAX];
    int mem_fd = -1;
    int dump_fd = -1;
    unsigned char buffer[4096];
    unsigned long long current_dump_offset = 0;

    if (snprintf(mem_path, sizeof(mem_path), "/proc/%d/mem", pid) >= (int)sizeof(mem_path)) {
        mc_log_error("Path file mem terlalu panjang.");
        return -1;
    }

    mem_fd = open(mem_path, O_RDONLY);
    if (mem_fd == -1) {
        mc_log_system_error("Gagal membuka /proc/<pid>/mem");
        return -1;
    }

    dump_fd = open(mem_dump_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (dump_fd == -1) {
        close(mem_fd);
        mc_log_system_error("Gagal membuka file mem.dump");
        return -1;
    }

    /*
     * Region diproses satu per satu. Sebelum membaca byte, kita catat posisi
     * awal region di `mem.dump` agar offset hasil dump bisa ditelusuri lagi.
     */
    for (size_t i = 0; i < region_count; ++i) {
        unsigned long long region_size;
        unsigned long long bytes_processed = 0;

        if (!regions[i].selected) {
            continue;
        }

        region_size = regions[i].end_address - regions[i].start_address;
        regions[i].dump_offset = current_dump_offset;
        regions[i].dumped_size = 0;
        snprintf(regions[i].dump_status, sizeof(regions[i].dump_status), "%s", "gagal");

        /*
         * `pread` dipakai supaya pembacaan selalu mengacu ke alamat virtual yang
         * benar tanpa bergantung pada posisi file descriptor sebelumnya.
         */
        while (bytes_processed < region_size) {
            size_t chunk_size = sizeof(buffer);
            ssize_t bytes_read;
            unsigned long long current_address = regions[i].start_address + bytes_processed;

            if ((unsigned long long)chunk_size > (region_size - bytes_processed)) {
                chunk_size = (size_t)(region_size - bytes_processed);
            }

            bytes_read = pread(mem_fd, buffer, chunk_size, (off_t)current_address);
            if (bytes_read < 0) {
                char message[256];

                if (errno == EINTR) {
                    continue;
                }

                snprintf(message,
                         sizeof(message),
                         "Gagal membaca region 0x%llx-0x%llx dari /proc/<pid>/mem",
                         regions[i].start_address,
                         regions[i].end_address);
                mc_log_system_error(message);
                break;
            }

            if (bytes_read == 0) {
                char message[256];

                snprintf(message,
                         sizeof(message),
                         "Pembacaan region 0x%llx-0x%llx berhenti sebelum selesai.",
                         regions[i].start_address,
                         regions[i].end_address);
                mc_log_error(message);
                break;
            }

            if (mc_write_all(dump_fd, buffer, (size_t)bytes_read) != 0) {
                char message[256];

                snprintf(message,
                         sizeof(message),
                         "Gagal menulis byte region 0x%llx-0x%llx ke mem.dump",
                         regions[i].start_address,
                         regions[i].end_address);
                mc_log_system_error(message);
                break;
            }

            bytes_processed += (unsigned long long)bytes_read;
            regions[i].dumped_size += (unsigned long long)bytes_read;
            current_dump_offset += (unsigned long long)bytes_read;
            *total_dumped_bytes_out += (unsigned long long)bytes_read;
        }

        if (regions[i].dumped_size == region_size) {
            snprintf(regions[i].dump_status, sizeof(regions[i].dump_status), "%s", "berhasil");
            ++(*dumped_regions_out);
        } else if (regions[i].dumped_size > 0) {
            snprintf(regions[i].dump_status, sizeof(regions[i].dump_status), "%s", "parsial");
            ++(*skipped_regions_out);
        } else {
            snprintf(regions[i].dump_status, sizeof(regions[i].dump_status), "%s", "gagal");
            ++(*skipped_regions_out);
        }
    }

    if (close(dump_fd) != 0) {
        close(mem_fd);
        mc_log_system_error("Gagal menutup file mem.dump");
        return -1;
    }

    if (close(mem_fd) != 0) {
        mc_log_system_error("Gagal menutup /proc/<pid>/mem");
        return -1;
    }

    return 0;
}

/*
 * File `mem.meta` menyimpan peta region dan hasil dump per region.
 *
 * Dengan cara ini, `mem.dump` boleh tetap berupa byte mentah yang sederhana,
 * sedangkan informasi interpretasinya tetap bisa dibaca manusia dari file teks.
 * Setiap entri region menunjukkan alamat asli region dan letak byte region itu
 * di dalam `mem.dump`.
 */
static int mc_write_memory_metadata_file(const char *path,
                                         pid_t pid,
                                         const char *checkpoint_flag,
                                         const char *snapshot_id,
                                         const char *timestamp,
                                         const mc_memory_region *regions,
                                         size_t region_count,
                                         size_t selected_count,
                                         size_t dumped_regions,
                                         size_t skipped_regions,
                                         unsigned long long total_dumped_bytes)
{
    FILE *file = fopen(path, "w");

    if (file == NULL) {
        mc_log_system_error("Gagal membuka file metadata memori");
        return -1;
    }

    if (fprintf(file,
                "jenis_dump=raw_memori\n"
                "checkpoint_flag=%s\n"
                "snapshot_id=%s\n"
                "pid_target=%d\n"
                "dibuat_pada=%s\n"
                "aturan_seleksi=region_writable_private_rw-p\n"
                "file_dump_memori=mem.dump\n"
                "format_dump=raw_berurutan_berdasarkan_region_terpilih\n"
                "jumlah_region=%zu\n"
                "jumlah_region_terpilih=%zu\n"
                "jumlah_region_dump_berhasil=%zu\n"
                "jumlah_region_dump_terlewati=%zu\n"
                "jumlah_byte_dump=%llu\n\n",
                checkpoint_flag,
                snapshot_id,
                pid,
                timestamp,
                region_count,
                selected_count,
                dumped_regions,
                skipped_regions,
                total_dumped_bytes) < 0) {
        fclose(file);
        mc_log_system_error("Gagal menulis header metadata memori");
        return -1;
    }

    for (size_t i = 0; i < region_count; ++i) {
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
                    "label=%s\n"
                    "status_dump=%s\n"
                    "offset_dump=0x%llx\n"
                    "ukuran_dump=%llu\n\n",
                    i,
                    regions[i].selected ? "ya" : "tidak",
                    regions[i].start_address,
                    regions[i].end_address,
                    region_size,
                    regions[i].permissions,
                    regions[i].offset,
                    label,
                    regions[i].dump_status,
                    regions[i].dump_offset,
                    regions[i].dumped_size) < 0) {
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
 * Ringkasan ini ditambahkan ke `checkpoint.info` agar status dump memori mentah
 * terlihat jelas tanpa harus membuka `mem.meta`.
 */
static int mc_append_memory_summary(const char *path,
                                    pid_t pid,
                                    const char *checkpoint_flag,
                                    const char *snapshot_id,
                                    const char *timestamp,
                                    size_t total_regions,
                                    size_t selected_regions,
                                    size_t dumped_regions,
                                    size_t skipped_regions,
                                    unsigned long long total_dumped_bytes)
{
    FILE *file = fopen(path, "a");

    if (file == NULL) {
        mc_log_system_error("Gagal membuka checkpoint.info untuk diringkas");
        return -1;
    }

    if (fprintf(file,
                "\n"
                "jenis_dump_memori=raw_memori\n"
                "checkpoint_flag=%s\n"
                "checkpoint_code=%s\n"
                "snapshot_id_memori=%s\n"
                "pid_target_memori=%d\n"
                "dibuat_pada_memori=%s\n"
                "status_snapshot=selesai\n"
                "file_peta_memori=mem.meta\n"
                "file_dump_memori=mem.dump\n"
                "jumlah_region=%zu\n"
                "jumlah_region_terpilih=%zu\n"
                "jumlah_region_dump_berhasil=%zu\n"
                "jumlah_region_dump_terlewati=%zu\n"
                "jumlah_byte_dump=%llu\n"
                "konsistensi_snapshot=register_dan_memori_diambil_dari_stop_yang_sama\n"
                "catatan_memori=Target dilepas kembali setelah dump-memory selesai. Restore parsial dapat dicoba dengan command restore.\n",
                checkpoint_flag,
                snapshot_id,
                snapshot_id,
                pid,
                timestamp,
                total_regions,
                selected_regions,
                dumped_regions,
                skipped_regions,
                total_dumped_bytes) < 0) {
        fclose(file);
        mc_log_system_error("Gagal menambahkan ringkasan dump memori");
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
    char message[160];
    char timestamp[32];
    char snapshot_id[MC_SNAPSHOT_ID_LEN];
    char checkpoint_dir[PATH_MAX];
    char metadata_path[PATH_MAX];
    char mem_meta_path[PATH_MAX];
    char mem_dump_path[PATH_MAX];
    mc_memory_region *regions = NULL;
    size_t region_count = 0;
    size_t selected_count = 0;
    size_t dumped_regions = 0;
    size_t skipped_regions = 0;
    unsigned long long total_dumped_bytes = 0;
    int result = 1;

    /*
     * Validasi awal memastikan ada target yang bisa dibaca sebelum command ini
     * membuka file `/proc`.
     */
    if (!ctx->snapshot_active) {
        mc_log_error("Belum ada snapshot aktif. Jalankan 'freeze' terlebih dahulu.");
        return 1;
    }

    if (ctx->target_pid <= 0) {
        mc_log_error("Belum ada target yang dipilih. Jalankan 'freeze <pid>' terlebih dahulu.");
        return 1;
    }

    if (!mc_process_exists(ctx->target_pid)) {
        mc_log_error("PID target yang dipilih sedang tidak berjalan.");
        return 1;
    }

    mc_print_section("Ringkasan dump memori");
    mc_print_kv_int("PID target", ctx->target_pid);
    mc_log_info("Memulai dump memori mentah.");

    /*
     * Pada titik ini target sudah berada dalam keadaan stop karena `freeze`
     * belum melepaskan tracer. Dengan begitu, register dan memori diambil dari
     * satu event snapshot yang lebih konsisten.
     */
    mc_print_kv_text("Flag checkpoint",
                     ctx->active_checkpoint_flag[0] != '\0' ?
                         ctx->active_checkpoint_flag :
                         "(tidak ada)");
    mc_log_info("Melanjutkan snapshot aktif yang sama.");
    snprintf(snapshot_id, sizeof(snapshot_id), "%s", ctx->active_snapshot_id);
    mc_format_timestamp(timestamp, sizeof(timestamp));

    /*
     * Setelah target berhenti, daftar region dimuat lebih dulu agar kita tahu
     * bagian mana yang akan dicatat ke metadata dan bagian mana yang akan
     * dibaca byte mentahnya.
     */
    mc_log_info("Membaca peta memori target.");
    if (mc_load_memory_regions(ctx->target_pid, &regions, &region_count) != 0) {
        goto cleanup;
    }

    if (region_count == 0) {
        mc_log_error("Tidak ada region memori yang berhasil diparsing dari /proc/<pid>/maps.");
        goto cleanup;
    }

    for (size_t i = 0; i < region_count; ++i) {
        if (regions[i].selected) {
            ++selected_count;
        }
    }

    snprintf(message,
             sizeof(message),
             "Metadata region siap: %zu region, %zu kandidat dump.",
             region_count,
             selected_count);
    mc_log_ok(message);

    mc_log_info("Menyiapkan file output dump memori.");
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

    if (mc_join_path(mem_dump_path, sizeof(mem_dump_path), checkpoint_dir, "mem.dump") != 0) {
        mc_log_error("Path file dump memori terlalu panjang.");
        goto cleanup;
    }

    /*
     * `mem.dump` ditulis lebih dulu, lalu `mem.meta` dan `checkpoint.info`
     * diperbarui dengan ukuran serta status hasil dump tersebut.
     */
    mc_log_info("Menyalin byte memori dari region terpilih.");
    if (mc_dump_selected_regions(ctx->target_pid,
                                 mem_dump_path,
                                 regions,
                                 region_count,
                                 &dumped_regions,
                                 &skipped_regions,
                                 &total_dumped_bytes) != 0) {
        goto cleanup;
    }

    snprintf(message,
             sizeof(message),
             "Dump mentah selesai: %zu region berhasil, %zu dilewati.",
             dumped_regions,
             skipped_regions);
    mc_log_ok(message);

    if (selected_count > 0 && dumped_regions == 0) {
        mc_log_error("Tidak ada region terpilih yang berhasil didump dari /proc/<pid>/mem.");
        goto cleanup;
    }

    mc_log_info("Menulis metadata hasil dump memori.");
    if (mc_write_memory_metadata_file(mem_meta_path,
                                      ctx->target_pid,
                                      ctx->active_checkpoint_flag,
                                      snapshot_id,
                                      timestamp,
                                      regions,
                                      region_count,
                                      selected_count,
                                      dumped_regions,
                                      skipped_regions,
                                      total_dumped_bytes) != 0) {
        goto cleanup;
    }

    if (mc_append_memory_summary(metadata_path,
                                 ctx->target_pid,
                                 ctx->active_checkpoint_flag,
                                 snapshot_id,
                                 timestamp,
                                 region_count,
                                 selected_count,
                                 dumped_regions,
                                 skipped_regions,
                                 total_dumped_bytes) != 0) {
        goto cleanup;
    }
    mc_log_ok("Metadata dump memori berhasil diperbarui.");

    mc_log_ok("Metadata dan dump memori berhasil disimpan.");
    mc_print_kv_text("Direktori", checkpoint_dir);
    mc_print_kv_size("Region diparsing", region_count);
    mc_print_kv_size("Region terpilih", selected_count);
    mc_print_kv_size("Dump berhasil", dumped_regions);
    mc_print_kv_size("Dump terlewati", skipped_regions);
    mc_print_kv_u64("Byte mentah", total_dumped_bytes);
    mc_print_kv_text("File", "mem.meta, mem.dump");
    mc_log_info("Restore masih bersifat parsial dan eksperimental.");
    result = 0;

cleanup:
    /*
     * Setelah dump-memory selesai, snapshot dianggap ditutup. Target dilepas
     * kembali baik saat berhasil maupun saat terjadi galat agar perilaku resume
     * tetap mudah dipahami.
     */
    if (mc_release_snapshot(ctx, true) != 0) {
        result = 1;
    }

    free(regions);
    return result;
}
