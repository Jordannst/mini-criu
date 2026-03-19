#include "mini_criu.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/stat.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

typedef struct {
    char label[MC_REGION_LABEL_LEN];
    char permissions[MC_REGION_PERMS_LEN];
    unsigned long long start_address;
    unsigned long long end_address;
    unsigned long long file_offset;
    unsigned long long dump_offset;
    unsigned long long dumped_size;
    bool selected;
    bool dump_complete;
    bool has_dump_bytes;
} mc_restore_region;

typedef enum {
    MC_MAP_SKIPPED = 0,
    MC_MAP_CANDIDATE,
    MC_MAP_RISKY
} mc_restore_mapping_kind;

typedef enum {
    MC_WRITEBACK_NONE = 0,
    MC_WRITEBACK_SAFE,
    MC_WRITEBACK_EXTENDED
    ,
    MC_WRITEBACK_STACK,
    MC_WRITEBACK_FILE_EXEC
} mc_restore_writeback_kind;

typedef struct {
    unsigned long long r15;
    unsigned long long r14;
    unsigned long long r13;
    unsigned long long r12;
    unsigned long long rbp;
    unsigned long long rbx;
    unsigned long long r11;
    unsigned long long r10;
    unsigned long long r9;
    unsigned long long r8;
    unsigned long long rax;
    unsigned long long rcx;
    unsigned long long rdx;
    unsigned long long rsi;
    unsigned long long rdi;
    unsigned long long orig_rax;
    unsigned long long rip;
    unsigned long long cs;
    unsigned long long eflags;
    unsigned long long rsp;
    unsigned long long ss;
    unsigned long long fs_base;
    unsigned long long gs_base;
    unsigned long long ds;
    unsigned long long es;
    unsigned long long fs;
    unsigned long long gs;
    bool loaded;
} mc_restore_registers;

typedef struct {
    pid_t pid;
    bool created;
    bool stopped;
    bool traced;
    bool regs_apply_attempted;
    bool regs_apply_succeeded;
    bool resume_attempted;
    bool resume_completed;
    bool resume_running_briefly;
    bool resume_stopped_again;
    bool resume_exited;
    bool resume_signaled;
    int resume_signal;
    int resume_exit_code;
    char resume_note[128];
} mc_restore_target;

typedef struct {
    bool available;
    bool has_runtime_regs;
    bool has_fault_address;
    bool stack_region_found;
    bool stack_region_restored;
    bool stack_region_attempted;
    unsigned long long rip;
    unsigned long long rsp;
    unsigned long long rbp;
    unsigned long long fault_address;
    int rip_region_index;
    int rsp_region_index;
    int rbp_region_index;
    int fault_region_index;
    char likely_cause[256];
} mc_restore_diagnostics;

typedef struct {
    mc_restore_mapping_kind kind;
    mc_restore_writeback_kind writeback_kind;
    unsigned long long restore_size;
    bool writeback_candidate;
    bool parent_window_active;
    bool target_window_ready;
    bool write_attempted;
    bool write_succeeded;
    unsigned long long bytes_written;
    const char *outcome_reason;
} mc_restore_mapping_entry;

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
    size_t mapping_candidate_regions;
    size_t mapping_risky_regions;
    size_t mapping_skipped_regions;
    unsigned long long mapping_candidate_bytes;
    size_t memory_attempted_regions;
    size_t memory_written_regions;
    size_t memory_failed_regions;
    size_t memory_skipped_regions;
    unsigned long long memory_written_bytes;
    size_t memory_safe_candidate_regions;
    size_t memory_extended_candidate_regions;
    size_t memory_stack_candidate_regions;
    size_t file_mapping_attempted_regions;
    size_t file_mapping_ready_regions;
    mc_restore_registers regs;
    mc_restore_target target;
    mc_restore_diagnostics diagnostics;
    mc_restore_mapping_entry *mapping_entries;
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
 * Mengisi satu field register berdasarkan pasangan `kunci=nilai` dari
 * `regs.dump`. Format file ini sama dengan yang ditulis oleh command `freeze`,
 * sehingga parser cukup mengikuti nama register yang sudah ada.
 */
static int mc_parse_register_field(mc_restore_registers *regs, const char *key, const char *value)
{
    unsigned long long parsed_value = 0;

    if (strcmp(key, "r15") == 0 || strcmp(key, "r14") == 0 || strcmp(key, "r13") == 0 ||
        strcmp(key, "r12") == 0 || strcmp(key, "rbp") == 0 || strcmp(key, "rbx") == 0 ||
        strcmp(key, "r11") == 0 || strcmp(key, "r10") == 0 || strcmp(key, "r9") == 0 ||
        strcmp(key, "r8") == 0 || strcmp(key, "rax") == 0 || strcmp(key, "rcx") == 0 ||
        strcmp(key, "rdx") == 0 || strcmp(key, "rsi") == 0 || strcmp(key, "rdi") == 0 ||
        strcmp(key, "orig_rax") == 0 || strcmp(key, "rip") == 0 || strcmp(key, "cs") == 0 ||
        strcmp(key, "eflags") == 0 || strcmp(key, "rsp") == 0 || strcmp(key, "ss") == 0 ||
        strcmp(key, "fs_base") == 0 || strcmp(key, "gs_base") == 0 || strcmp(key, "ds") == 0 ||
        strcmp(key, "es") == 0 || strcmp(key, "fs") == 0 || strcmp(key, "gs") == 0) {
        if (mc_parse_ull_value(value, &parsed_value) != 0) {
            return -1;
        }
    } else {
        return 1;
    }

    if (strcmp(key, "r15") == 0) {
        regs->r15 = parsed_value;
    } else if (strcmp(key, "r14") == 0) {
        regs->r14 = parsed_value;
    } else if (strcmp(key, "r13") == 0) {
        regs->r13 = parsed_value;
    } else if (strcmp(key, "r12") == 0) {
        regs->r12 = parsed_value;
    } else if (strcmp(key, "rbp") == 0) {
        regs->rbp = parsed_value;
    } else if (strcmp(key, "rbx") == 0) {
        regs->rbx = parsed_value;
    } else if (strcmp(key, "r11") == 0) {
        regs->r11 = parsed_value;
    } else if (strcmp(key, "r10") == 0) {
        regs->r10 = parsed_value;
    } else if (strcmp(key, "r9") == 0) {
        regs->r9 = parsed_value;
    } else if (strcmp(key, "r8") == 0) {
        regs->r8 = parsed_value;
    } else if (strcmp(key, "rax") == 0) {
        regs->rax = parsed_value;
    } else if (strcmp(key, "rcx") == 0) {
        regs->rcx = parsed_value;
    } else if (strcmp(key, "rdx") == 0) {
        regs->rdx = parsed_value;
    } else if (strcmp(key, "rsi") == 0) {
        regs->rsi = parsed_value;
    } else if (strcmp(key, "rdi") == 0) {
        regs->rdi = parsed_value;
    } else if (strcmp(key, "orig_rax") == 0) {
        regs->orig_rax = parsed_value;
    } else if (strcmp(key, "rip") == 0) {
        regs->rip = parsed_value;
    } else if (strcmp(key, "cs") == 0) {
        regs->cs = parsed_value;
    } else if (strcmp(key, "eflags") == 0) {
        regs->eflags = parsed_value;
    } else if (strcmp(key, "rsp") == 0) {
        regs->rsp = parsed_value;
    } else if (strcmp(key, "ss") == 0) {
        regs->ss = parsed_value;
    } else if (strcmp(key, "fs_base") == 0) {
        regs->fs_base = parsed_value;
    } else if (strcmp(key, "gs_base") == 0) {
        regs->gs_base = parsed_value;
    } else if (strcmp(key, "ds") == 0) {
        regs->ds = parsed_value;
    } else if (strcmp(key, "es") == 0) {
        regs->es = parsed_value;
    } else if (strcmp(key, "fs") == 0) {
        regs->fs = parsed_value;
    } else if (strcmp(key, "gs") == 0) {
        regs->gs = parsed_value;
    }

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
 * Menentukan apakah satu region merupakan kandidat remap, harus dilewati, atau
 * perlu penanganan khusus terlebih dahulu.
 *
 * Aturan ini sengaja sederhana:
 * - kandidat: region dipilih, punya dump lengkap, ukuran dump cocok, dan izin `rw-p`
 * - berisiko: region dipilih tetapi dump parsial/tidak lengkap atau izinnya tidak cocok
 * - dilewati: region tidak dipilih atau tidak punya byte dump
 */
static mc_restore_mapping_kind mc_classify_mapping_region(const mc_restore_region *region,
                                                          unsigned long long *restore_size_out)
{
    unsigned long long region_size = region->end_address - region->start_address;

    *restore_size_out = region_size;

    if (!region->selected || !region->has_dump_bytes) {
        return MC_MAP_SKIPPED;
    }

    if (!region->dump_complete || region->dumped_size != region_size) {
        return MC_MAP_RISKY;
    }

    if (strcmp(region->permissions, "rw-p") != 0) {
        return MC_MAP_RISKY;
    }

    return MC_MAP_CANDIDATE;
}

/*
 * Membuat rencana mapping restore dari metadata region yang sudah diparsing.
 *
 * Rencana ini belum melakukan `mmap`, tetapi sudah menjelaskan region mana
 * yang realistis untuk dipetakan kembali dan berapa byte yang nantinya perlu
 * ditulis ke target restore.
 */
static int mc_build_restore_mapping_plan(mc_restore_plan *plan)
{
    mc_restore_mapping_entry *entries = NULL;

    if (plan->region_count == 0) {
        return 0;
    }

    entries = calloc(plan->region_count, sizeof(*entries));
    if (entries == NULL) {
        mc_log_error("Gagal mengalokasikan memori untuk rencana mapping restore.");
        return -1;
    }

    for (size_t i = 0; i < plan->region_count; ++i) {
        unsigned long long restore_size = 0;

        entries[i].kind = mc_classify_mapping_region(&plan->regions[i], &restore_size);
        entries[i].restore_size = restore_size;

        if (entries[i].kind == MC_MAP_CANDIDATE) {
            entries[i].outcome_reason = "kandidat mapping valid";
            ++plan->mapping_candidate_regions;
            plan->mapping_candidate_bytes += restore_size;
        } else if (entries[i].kind == MC_MAP_RISKY) {
            entries[i].outcome_reason = "dump belum lengkap atau izin region tidak cocok";
            ++plan->mapping_risky_regions;
        } else {
            entries[i].outcome_reason = "region tidak dipilih atau tidak punya byte dump";
            ++plan->mapping_skipped_regions;
        }
    }

    plan->mapping_entries = entries;
    return 0;
}

/*
 * Izin region dari metadata checkpoint diterjemahkan ke flag `mmap` agar
 * region executable file-backed bisa dipetakan kembali dari file aslinya.
 */
static int mc_region_permissions_to_prot(const char *permissions)
{
    int prot = 0;

    if (permissions[0] == 'r') {
        prot |= PROT_READ;
    }
    if (permissions[1] == 'w') {
        prot |= PROT_WRITE;
    }
    if (permissions[2] == 'x') {
        prot |= PROT_EXEC;
    }

    return prot;
}

/*
 * Mencari region checkpoint yang mencakup satu alamat runtime.
 *
 * Helper ini dipakai saat diagnosis resume agar alamat seperti RIP, RSP, RBP,
 * dan alamat fault bisa dikaitkan dengan metadata checkpoint yang sudah ada.
 */
static int mc_find_region_index_for_address(const mc_restore_plan *plan, unsigned long long address)
{
    for (size_t i = 0; i < plan->region_count; ++i) {
        if (address >= plan->regions[i].start_address && address < plan->regions[i].end_address) {
            return (int)i;
        }
    }

    return -1;
}

/*
 * Menerjemahkan status restore region ke label singkat yang mudah dibaca
 * pengguna saat diagnosis crash ditampilkan.
 */
static const char *mc_describe_region_restore_state(const mc_restore_plan *plan, int region_index)
{
    const mc_restore_mapping_entry *entry = NULL;

    if (region_index < 0 || (size_t)region_index >= plan->region_count) {
        return "tidak ada di metadata checkpoint";
    }

    entry = &plan->mapping_entries[region_index];

    if (entry->writeback_kind == MC_WRITEBACK_FILE_EXEC) {
        if (entry->writeback_candidate && entry->target_window_ready) {
            return "dipetakan dari file asli";
        }
        return "region eksekusi belum berhasil dipetakan";
    }

    if (entry->write_succeeded) {
        return "sudah ditulis kembali";
    }

    if (entry->write_attempted) {
        return "sudah dicoba tetapi belum berhasil";
    }

    if (entry->kind == MC_MAP_RISKY) {
        return "ditandai berisiko dan belum dipulihkan";
    }

    if (entry->kind == MC_MAP_SKIPPED || entry->writeback_kind == MC_WRITEBACK_NONE) {
        return "dilewati dan belum dipulihkan";
    }

    return "belum ditulis kembali";
}

/*
 * Diagnosis runtime perlu tahu apakah suatu region sudah cukup siap dipakai
 * target, baik lewat write-back byte checkpoint maupun lewat mapping file asli.
 */
static bool mc_region_is_runtime_ready(const mc_restore_plan *plan, int region_index)
{
    const mc_restore_mapping_entry *entry = NULL;

    if (region_index < 0 || (size_t)region_index >= plan->region_count) {
        return false;
    }

    entry = &plan->mapping_entries[region_index];

    if (entry->writeback_kind == MC_WRITEBACK_FILE_EXEC) {
        return entry->writeback_candidate && entry->target_window_ready;
    }

    return entry->write_succeeded;
}

/*
 * Mengumpulkan konteks crash atau stop setelah resume eksperimen.
 *
 * Informasi ini tidak memperbaiki restore, tetapi membantu menjelaskan kenapa
 * target masih gagal: misalnya RIP mengarah ke region yang belum dipulihkan
 * atau pointer stack masih berada pada region stack yang sengaja dilewati.
 */
static void mc_collect_resume_diagnostics(mc_restore_plan *plan)
{
    struct user_regs_struct regs;
    siginfo_t signal_info;
    bool has_stack_warning = false;
    bool has_rip_warning = false;
    bool has_rsp_warning = false;

    if (!plan->target.created || !plan->target.traced || !plan->target.resume_stopped_again) {
        return;
    }

    plan->diagnostics.available = true;

    if (ptrace(PTRACE_GETREGS, plan->target.pid, NULL, &regs) == 0) {
        plan->diagnostics.has_runtime_regs = true;
        plan->diagnostics.rip = regs.rip;
        plan->diagnostics.rsp = regs.rsp;
        plan->diagnostics.rbp = regs.rbp;
        plan->diagnostics.rip_region_index = mc_find_region_index_for_address(plan, regs.rip);
        plan->diagnostics.rsp_region_index = mc_find_region_index_for_address(plan, regs.rsp);
        plan->diagnostics.rbp_region_index = mc_find_region_index_for_address(plan, regs.rbp);
    }

    memset(&signal_info, 0, sizeof(signal_info));
    if (ptrace(PTRACE_GETSIGINFO, plan->target.pid, NULL, &signal_info) == 0) {
        plan->diagnostics.has_fault_address = true;
        plan->diagnostics.fault_address = (unsigned long long)(uintptr_t)signal_info.si_addr;
        plan->diagnostics.fault_region_index =
            mc_find_region_index_for_address(plan, plan->diagnostics.fault_address);
    }

    for (size_t i = 0; i < plan->region_count; ++i) {
        if (strcmp(plan->regions[i].label, "[stack]") == 0) {
            plan->diagnostics.stack_region_found = true;
            plan->diagnostics.stack_region_attempted = plan->mapping_entries[i].write_attempted;
            plan->diagnostics.stack_region_restored = plan->mapping_entries[i].write_succeeded;
            break;
        }
    }

    if (plan->diagnostics.stack_region_found && !plan->diagnostics.stack_region_restored) {
        has_stack_warning = true;
    }

    if (plan->diagnostics.rip_region_index >= 0 &&
        !mc_region_is_runtime_ready(plan, plan->diagnostics.rip_region_index)) {
        has_rip_warning = true;
    }

    if (plan->diagnostics.rsp_region_index >= 0 &&
        !mc_region_is_runtime_ready(plan, plan->diagnostics.rsp_region_index)) {
        has_rsp_warning = true;
    }

    if (plan->target.resume_signal == SIGSEGV) {
        if (has_stack_warning && has_rip_warning) {
            snprintf(plan->diagnostics.likely_cause,
                     sizeof(plan->diagnostics.likely_cause),
                     "%s",
                     "RIP mengarah ke region yang belum dipulihkan dan stack checkpoint juga masih belum dipulihkan.");
        } else if (has_stack_warning && has_rsp_warning) {
            snprintf(plan->diagnostics.likely_cause,
                     sizeof(plan->diagnostics.likely_cause),
                     "%s",
                     "Pointer stack masih berada di region stack yang belum dipulihkan.");
        } else if (has_rip_warning) {
            snprintf(plan->diagnostics.likely_cause,
                     sizeof(plan->diagnostics.likely_cause),
                     "%s",
                     "RIP mengarah ke region yang belum dipulihkan pada target restore.");
        } else if (plan->diagnostics.has_fault_address &&
                   plan->diagnostics.fault_region_index < 0) {
            snprintf(plan->diagnostics.likely_cause,
                     sizeof(plan->diagnostics.likely_cause),
                     "%s",
                     "Alamat fault berada di luar region checkpoint yang berhasil dimuat.");
        } else {
            snprintf(plan->diagnostics.likely_cause,
                     sizeof(plan->diagnostics.likely_cause),
                     "%s",
                     "State register dan memori target masih belum cukup konsisten untuk melanjutkan eksekusi.");
        }
    } else {
        snprintf(plan->diagnostics.likely_cause,
                 sizeof(plan->diagnostics.likely_cause),
                 "%s",
                 "Target berhenti lagi setelah resume, tetapi penyebab detailnya masih terbatas pada observasi dasar.");
    }
}

/*
 * Klasifikasi pemulihan dibuat bertahap:
 * - aman: heap atau region anonim private
 * - tambahan terkontrol: region writable private lain
 * - stack: dicoba secara khusus karena RSP/RBP sangat bergantung padanya
 * - file-eksekusi: dicoba lewat mapping file asli untuk membantu RIP
 */
static mc_restore_writeback_kind mc_choose_writeback_kind(const mc_restore_region *region,
                                                          const mc_restore_mapping_entry *entry,
                                                          unsigned long long page_size,
                                                          const char **reason_out)
{
    *reason_out = "bukan kandidat write-back";

    if (entry->restore_size == 0) {
        return MC_WRITEBACK_NONE;
    }

    if ((region->start_address % page_size) != 0 || (entry->restore_size % page_size) != 0) {
        *reason_out = "alamat atau ukuran tidak selaras halaman";
        return MC_WRITEBACK_NONE;
    }

    if (entry->kind == MC_MAP_CANDIDATE) {
        if (strcmp(region->label, "[stack]") == 0) {
            *reason_out = "region stack akan dicoba secara khusus";
            return MC_WRITEBACK_STACK;
        }

        if (strcmp(region->label, "(anonim)") == 0 || strcmp(region->label, "[heap]") == 0) {
            *reason_out = "kandidat aman";
            return MC_WRITEBACK_SAFE;
        }

        *reason_out = "kandidat tambahan yang masih dicoba secara terkontrol";
        return MC_WRITEBACK_EXTENDED;
    }

    if (entry->kind == MC_MAP_SKIPPED &&
        region->label[0] == '/' &&
        strchr(region->permissions, 'x') != NULL &&
        (region->file_offset % page_size) == 0) {
        *reason_out = "region eksekusi file-backed akan dicoba lewat file asli";
        return MC_WRITEBACK_FILE_EXEC;
    }

    return MC_WRITEBACK_NONE;
}

/*
 * Sebelum child restore dibuat, parent mencoba membuka "jendela alamat"
 * anonim pada alamat asli checkpoint. Jika berhasil, child hasil `fork`
 * akan mewarisi mapping itu dan parent bisa melepas salinannya kembali.
 *
 * Cara ini dipilih sebagai langkah paling kecil untuk memulai write-back
 * memori tanpa langsung membangun injeksi `mmap` jarak jauh yang lebih rumit.
 */
static int mc_prepare_parent_restore_windows(mc_restore_plan *plan)
{
    long page_size_long = sysconf(_SC_PAGESIZE);
    unsigned long long page_size = 0;

    if (page_size_long <= 0) {
        mc_log_error("Ukuran halaman memori sistem tidak tersedia.");
        return -1;
    }

    page_size = (unsigned long long)page_size_long;

    for (size_t i = 0; i < plan->region_count; ++i) {
        void *mapped = NULL;
        mc_restore_mapping_entry *entry = &plan->mapping_entries[i];
        const mc_restore_region *region = &plan->regions[i];
        const char *reason = NULL;
        int mapped_fd = -1;

        entry->writeback_kind = mc_choose_writeback_kind(region, entry, page_size, &reason);
        entry->outcome_reason = reason;

        if (entry->writeback_kind == MC_WRITEBACK_NONE) {
            continue;
        }

        if (entry->writeback_kind == MC_WRITEBACK_SAFE) {
            ++plan->memory_safe_candidate_regions;
        } else if (entry->writeback_kind == MC_WRITEBACK_EXTENDED) {
            ++plan->memory_extended_candidate_regions;
        } else if (entry->writeback_kind == MC_WRITEBACK_STACK) {
            ++plan->memory_stack_candidate_regions;
        } else if (entry->writeback_kind == MC_WRITEBACK_FILE_EXEC) {
            ++plan->file_mapping_attempted_regions;
        }

        if (entry->writeback_kind == MC_WRITEBACK_FILE_EXEC) {
            mapped_fd = open(region->label, O_RDONLY);
            if (mapped_fd < 0) {
                entry->outcome_reason = "file asli region eksekusi tidak bisa dibuka";
                continue;
            }

            mapped = mmap((void *)(uintptr_t)region->start_address,
                          (size_t)entry->restore_size,
                          mc_region_permissions_to_prot(region->permissions),
                          MAP_PRIVATE | MAP_FIXED_NOREPLACE,
                          mapped_fd,
                          (off_t)region->file_offset);
            close(mapped_fd);
        } else {
            mapped = mmap((void *)(uintptr_t)region->start_address,
                          (size_t)entry->restore_size,
                          PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
                          -1,
                          0);
        }

        if (mapped == MAP_FAILED) {
            if (entry->writeback_kind == MC_WRITEBACK_FILE_EXEC) {
                entry->outcome_reason = "region eksekusi tidak bisa dipetakan dari file asli";
            } else {
                entry->outcome_reason = "alamat target tidak bisa disiapkan tanpa bentrok";
            }
            continue;
        }

        entry->writeback_candidate = true;
        entry->parent_window_active = true;
        if (entry->writeback_kind == MC_WRITEBACK_SAFE) {
            entry->outcome_reason = "kandidat aman siap ditulis kembali";
        } else if (entry->writeback_kind == MC_WRITEBACK_EXTENDED) {
            entry->outcome_reason = "kandidat tambahan siap dicoba";
        } else if (entry->writeback_kind == MC_WRITEBACK_STACK) {
            entry->outcome_reason = "region stack siap dicoba";
        } else {
            ++plan->file_mapping_ready_regions;
            entry->outcome_reason = "region eksekusi berhasil dipetakan dari file asli";
        }
    }

    return 0;
}

/*
 * Setelah `fork`, parent tidak lagi memerlukan salinan mapping sementaranya.
 * Mapping di child tetap ada karena sudah diwariskan saat proses dibuat.
 */
static int mc_release_parent_restore_windows(mc_restore_plan *plan, bool mark_target_ready)
{
    for (size_t i = 0; i < plan->region_count; ++i) {
        mc_restore_mapping_entry *entry = &plan->mapping_entries[i];

        if (!entry->parent_window_active) {
            continue;
        }

        if (munmap((void *)(uintptr_t)plan->regions[i].start_address, (size_t)entry->restore_size) != 0) {
            mc_log_system_error("Gagal melepas mapping sementara di parent");
            return -1;
        }

        entry->parent_window_active = false;
        if (mark_target_ready) {
            entry->target_window_ready = true;
        }
    }

    return 0;
}

/*
 * `regs.dump` menyimpan snapshot register CPU dalam bentuk teks `kunci=nilai`.
 *
 * Data ini dibaca lebih dulu agar rencana restore tidak hanya mengetahui
 * susunan memori, tetapi juga titik eksekusi proses yang nanti perlu dipulihkan.
 */
static int mc_load_register_dump(const char *path, mc_restore_plan *plan)
{
    FILE *file = NULL;
    char line[512];
    bool has_dump_kind = false;
    bool has_pid = false;
    bool has_rip = false;
    bool has_rsp = false;
    bool has_rbp = false;

    file = fopen(path, "r");
    if (file == NULL) {
        mc_log_system_error("Gagal membuka regs.dump");
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

        if (strcmp(key, "jenis_dump") == 0) {
            if (strcmp(value, "register_cpu_x86_64") != 0) {
                fclose(file);
                mc_log_error("Jenis dump register pada regs.dump tidak didukung.");
                return -1;
            }
            has_dump_kind = true;
        } else if (strcmp(key, "pid_target") == 0) {
            pid_t regs_pid = -1;

            if (!mc_parse_pid(value, &regs_pid)) {
                fclose(file);
                mc_log_error("Nilai pid_target pada regs.dump tidak valid.");
                return -1;
            }

            if (plan->pid_target > 0 && plan->pid_target != regs_pid) {
                fclose(file);
                mc_log_error("PID target pada regs.dump tidak cocok dengan metadata checkpoint.");
                return -1;
            }

            plan->pid_target = regs_pid;
            has_pid = true;
        } else {
            int parse_status = mc_parse_register_field(&plan->regs, key, value);

            if (parse_status < 0) {
                fclose(file);
                mc_log_error("Nilai register pada regs.dump tidak valid.");
                return -1;
            }

            if (parse_status == 0) {
                if (strcmp(key, "rip") == 0) {
                    has_rip = true;
                } else if (strcmp(key, "rsp") == 0) {
                    has_rsp = true;
                } else if (strcmp(key, "rbp") == 0) {
                    has_rbp = true;
                }
            }
        }
    }

    if (fclose(file) != 0) {
        mc_log_system_error("Gagal menutup regs.dump");
        return -1;
    }

    if (!has_dump_kind || !has_pid || !has_rip || !has_rsp || !has_rbp) {
        mc_log_error("regs.dump belum memuat data register penting yang dibutuhkan.");
        return -1;
    }

    plan->regs.loaded = true;
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
                } else if (strcmp(key, "offset") == 0) {
                    if (mc_parse_ull_value(value, &current_region.file_offset) != 0) {
                        fclose(file);
                        mc_log_error("Nilai offset file region pada mem.meta tidak valid.");
                        return -1;
                    }
                } else if (strcmp(key, "offset_dump") == 0) {
                    if (mc_parse_ull_value(value, &current_region.dump_offset) != 0) {
                        fclose(file);
                        mc_log_error("Nilai offset_dump pada mem.meta tidak valid.");
                        return -1;
                    }
                } else if (strcmp(key, "izin") == 0) {
                    if (snprintf(current_region.permissions,
                                 sizeof(current_region.permissions),
                                 "%s",
                                 value) >= (int)sizeof(current_region.permissions)) {
                        fclose(file);
                        mc_log_error("Nilai izin pada mem.meta terlalu panjang.");
                        return -1;
                    }
                } else if (strcmp(key, "label") == 0) {
                    if (snprintf(current_region.label,
                                 sizeof(current_region.label),
                                 "%s",
                                 value) >= (int)sizeof(current_region.label)) {
                        fclose(file);
                        mc_log_error("Nilai label pada mem.meta terlalu panjang.");
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
 * Restore nantinya membutuhkan proses target baru sebagai tempat penulisan
 * register dan memori hasil checkpoint.
 *
 * Child ini meminta untuk ditrace oleh parent melalui `PTRACE_TRACEME`, lalu
 * berhenti dengan `SIGSTOP`. Dengan begitu, parent bisa langsung menyiapkan
 * register target tanpa membuat arsitektur baru yang besar.
 */
static int mc_create_restore_target(mc_restore_plan *plan)
{
    int wait_status = 0;
    pid_t child_pid = fork();

    if (child_pid < 0) {
        mc_release_parent_restore_windows(plan, false);
        mc_log_system_error("Gagal membuat proses target restore");
        return -1;
    }

    if (child_pid == 0) {
        if (ptrace(PTRACE_TRACEME, 0, NULL, NULL) == -1) {
            _exit(1);
        }
        raise(SIGSTOP);
        pause();
        _exit(0);
    }

    if (mc_release_parent_restore_windows(plan, true) != 0) {
        kill(child_pid, SIGKILL);
        waitpid(child_pid, NULL, 0);
        return -1;
    }

    if (waitpid(child_pid, &wait_status, WUNTRACED) == -1) {
        kill(child_pid, SIGKILL);
        waitpid(child_pid, NULL, 0);
        mc_log_system_error("Gagal menunggu target restore berhenti");
        return -1;
    }

    if (!WIFSTOPPED(wait_status)) {
        kill(child_pid, SIGKILL);
        waitpid(child_pid, NULL, 0);
        mc_log_error("Target restore tidak masuk ke status stop yang diharapkan.");
        return -1;
    }

    plan->target.pid = child_pid;
    plan->target.created = true;
    plan->target.stopped = true;
    plan->target.traced = true;
    return 0;
}

/*
 * Child kerangka restore dibersihkan kembali setelah inspeksi selesai.
 *
 * Tahap ini belum menulis register atau memori ke child tersebut, jadi proses
 * sementara tidak perlu dibiarkan hidup setelah statusnya berhasil dilaporkan.
 */
static int mc_cleanup_restore_target(mc_restore_plan *plan, bool announce)
{
    if (!plan->target.created) {
        return 0;
    }

    if (kill(plan->target.pid, SIGKILL) == -1 && errno != ESRCH) {
        mc_log_system_error("Gagal menghentikan target restore sementara");
        return -1;
    }

    if (waitpid(plan->target.pid, NULL, 0) == -1 && errno != ECHILD) {
        mc_log_system_error("Gagal membersihkan target restore sementara");
        return -1;
    }

    if (announce) {
        puts("Kerangka target restore sementara sudah dibersihkan kembali.");
    }

    plan->target.created = false;
    plan->target.stopped = false;
    plan->target.traced = false;
    plan->target.regs_apply_attempted = false;
    plan->target.regs_apply_succeeded = false;
    plan->target.resume_attempted = false;
    plan->target.resume_completed = false;
    plan->target.resume_running_briefly = false;
    plan->target.resume_stopped_again = false;
    plan->target.resume_exited = false;
    plan->target.resume_signaled = false;
    plan->target.resume_signal = 0;
    plan->target.resume_exit_code = 0;
    plan->target.resume_note[0] = '\0';
    return 0;
}

/*
 * Register checkpoint dihubungkan ke target restore dengan langkah terkecil
 * yang masih bermakna: parent membaca register child saat ini, lalu menimpa
 * register umum dan pointer eksekusi dari checkpoint.
 *
 * Register segment dan base tidak disentuh pada tahap ini agar percobaan tetap
 * konservatif. Tanpa pemulihan memori, target memang belum aman untuk dijalankan.
 */
static int mc_apply_checkpoint_registers(mc_restore_plan *plan)
{
    struct user_regs_struct target_regs;

    if (!plan->target.created || !plan->target.traced) {
        mc_log_error("Target restore belum siap untuk penerapan register.");
        return -1;
    }

    if (!plan->regs.loaded) {
        mc_log_error("Data register checkpoint belum berhasil dimuat.");
        return -1;
    }

    plan->target.regs_apply_attempted = true;

    if (ptrace(PTRACE_GETREGS, plan->target.pid, NULL, &target_regs) == -1) {
        mc_log_system_error("Gagal membaca register awal target restore");
        return -1;
    }

    /*
     * Hanya register umum yang diterapkan pada tahap ini. Nilai seperti RIP
     * dan RSP memang ditulis, tetapi target tidak di-resume karena memory map
     * checkpoint belum dipulihkan.
     */
    target_regs.r15 = plan->regs.r15;
    target_regs.r14 = plan->regs.r14;
    target_regs.r13 = plan->regs.r13;
    target_regs.r12 = plan->regs.r12;
    target_regs.rbp = plan->regs.rbp;
    target_regs.rbx = plan->regs.rbx;
    target_regs.r11 = plan->regs.r11;
    target_regs.r10 = plan->regs.r10;
    target_regs.r9 = plan->regs.r9;
    target_regs.r8 = plan->regs.r8;
    target_regs.rax = plan->regs.rax;
    target_regs.rcx = plan->regs.rcx;
    target_regs.rdx = plan->regs.rdx;
    target_regs.rsi = plan->regs.rsi;
    target_regs.rdi = plan->regs.rdi;
    target_regs.orig_rax = plan->regs.orig_rax;
    target_regs.rip = plan->regs.rip;
    target_regs.eflags = plan->regs.eflags;
    target_regs.rsp = plan->regs.rsp;

    if (ptrace(PTRACE_SETREGS, plan->target.pid, NULL, &target_regs) == -1) {
        mc_log_system_error("Gagal menerapkan register checkpoint ke target restore");
        return -1;
    }

    plan->target.regs_apply_succeeded = true;
    return 0;
}

/*
 * Eksperimen resume ini sengaja dibuat singkat dan terkontrol.
 *
 * Target di-continue sebentar untuk melihat apakah proses:
 * - langsung berhenti lagi
 * - keluar
 * - crash karena sinyal
 * - atau sempat berjalan singkat lalu dipaksa stop kembali
 *
 * Hasilnya hanya dipakai sebagai observasi awal. Ini belum membuktikan bahwa
 * restore sudah benar atau proses sudah aman dijalankan penuh.
 */
static int mc_run_controlled_resume_experiment(mc_restore_plan *plan)
{
    const int poll_count = 5;
    const struct timespec poll_delay = {.tv_sec = 0, .tv_nsec = 50000000L};
    int wait_status = 0;

    if (!plan->target.created || !plan->target.traced) {
        mc_log_error("Target restore belum siap untuk eksperimen resume.");
        return -1;
    }

    plan->target.resume_attempted = true;

    if (ptrace(PTRACE_CONT, plan->target.pid, NULL, NULL) == -1) {
        mc_log_system_error("Gagal melanjutkan target restore untuk eksperimen resume");
        snprintf(plan->target.resume_note,
                 sizeof(plan->target.resume_note),
                 "%s",
                 "gagal menjalankan PTRACE_CONT");
        return -1;
    }

    plan->target.stopped = false;

    for (int i = 0; i < poll_count; ++i) {
        pid_t wait_result = waitpid(plan->target.pid, &wait_status, WNOHANG | WUNTRACED);

        if (wait_result == -1) {
            mc_log_system_error("Gagal mengamati status target restore setelah resume");
            snprintf(plan->target.resume_note,
                     sizeof(plan->target.resume_note),
                     "%s",
                     "gagal membaca status target setelah resume");
            return -1;
        }

        if (wait_result == 0) {
            nanosleep(&poll_delay, NULL);
            continue;
        }

        plan->target.resume_completed = true;

        if (WIFSTOPPED(wait_status)) {
            plan->target.stopped = true;
            plan->target.resume_stopped_again = true;
            plan->target.resume_signal = WSTOPSIG(wait_status);
            mc_collect_resume_diagnostics(plan);
            snprintf(plan->target.resume_note,
                     sizeof(plan->target.resume_note),
                     "target berhenti lagi oleh sinyal %d",
                     plan->target.resume_signal);
            return 0;
        }

        if (WIFEXITED(wait_status)) {
            plan->target.created = false;
            plan->target.traced = false;
            plan->target.resume_exited = true;
            plan->target.resume_exit_code = WEXITSTATUS(wait_status);
            snprintf(plan->target.resume_note,
                     sizeof(plan->target.resume_note),
                     "target keluar dengan kode %d",
                     plan->target.resume_exit_code);
            return 0;
        }

        if (WIFSIGNALED(wait_status)) {
            plan->target.created = false;
            plan->target.traced = false;
            plan->target.resume_signaled = true;
            plan->target.resume_signal = WTERMSIG(wait_status);
            snprintf(plan->target.resume_note,
                     sizeof(plan->target.resume_note),
                     "target berhenti karena sinyal fatal %d",
                     plan->target.resume_signal);
            return 0;
        }
    }

    plan->target.resume_running_briefly = true;

    if (kill(plan->target.pid, SIGSTOP) == -1) {
        mc_log_system_error("Gagal menghentikan kembali target restore setelah resume singkat");
        snprintf(plan->target.resume_note,
                 sizeof(plan->target.resume_note),
                 "%s",
                 "target sempat berjalan, tetapi gagal dihentikan kembali");
        return -1;
    }

    if (waitpid(plan->target.pid, &wait_status, WUNTRACED) == -1) {
        mc_log_system_error("Gagal menunggu target restore berhenti kembali");
        snprintf(plan->target.resume_note,
                 sizeof(plan->target.resume_note),
                 "%s",
                 "target sempat berjalan, tetapi status stop akhir tidak terbaca");
        return -1;
    }

    if (!WIFSTOPPED(wait_status)) {
        snprintf(plan->target.resume_note,
                 sizeof(plan->target.resume_note),
                 "%s",
                 "target sempat berjalan, tetapi tidak kembali ke status stop yang diharapkan");
        return 0;
    }

    plan->target.stopped = true;
    plan->target.resume_stopped_again = true;
    plan->target.resume_signal = WSTOPSIG(wait_status);
    snprintf(plan->target.resume_note,
             sizeof(plan->target.resume_note),
             "target sempat berjalan singkat lalu dihentikan kembali");
    return 0;
}

/*
 * Byte mentah dari `mem.dump` ditulis kembali melalui `/proc/<pid>/mem`.
 *
 * Tahap ini hanya mencoba region yang sudah:
 * - lolos klasifikasi kandidat mapping
 * - masuk kelompok aman atau tambahan yang masih terkontrol
 * - berhasil disiapkan sebagai mapping anonim di child restore
 *
 * Dengan batasan ini, tool mulai melakukan write-back nyata tetapi tetap
 * jujur bahwa hasilnya masih parsial dan belum cukup untuk restore penuh.
 */
static int mc_write_memory_back_to_restore_target(const char *mem_dump_path, mc_restore_plan *plan)
{
    char target_mem_path[PATH_MAX];
    unsigned char buffer[16384];
    int dump_fd = -1;
    int target_mem_fd = -1;

    if (!plan->target.created || !plan->target.stopped) {
        mc_log_error("Target restore belum siap untuk write-back memori.");
        return -1;
    }

    if (snprintf(target_mem_path,
                 sizeof(target_mem_path),
                 "/proc/%d/mem",
                 plan->target.pid) >= (int)sizeof(target_mem_path)) {
        mc_log_error("Path memori target restore terlalu panjang.");
        return -1;
    }

    dump_fd = open(mem_dump_path, O_RDONLY);
    if (dump_fd < 0) {
        mc_log_system_error("Gagal membuka mem.dump");
        return -1;
    }

    target_mem_fd = open(target_mem_path, O_RDWR);
    if (target_mem_fd < 0) {
        close(dump_fd);
        mc_log_system_error("Gagal membuka memori target restore");
        return -1;
    }

    for (size_t i = 0; i < plan->region_count; ++i) {
        mc_restore_mapping_entry *entry = &plan->mapping_entries[i];
        const mc_restore_region *region = &plan->regions[i];
        unsigned long long written_for_region = 0;
        bool region_failed = false;

        if (!entry->writeback_candidate || !entry->target_window_ready ||
            entry->writeback_kind == MC_WRITEBACK_FILE_EXEC) {
            continue;
        }

        entry->write_attempted = true;
        ++plan->memory_attempted_regions;

        while (written_for_region < entry->restore_size) {
            size_t chunk_size = sizeof(buffer);
            ssize_t bytes_read = 0;
            ssize_t bytes_written = 0;
            unsigned long long remaining = entry->restore_size - written_for_region;

            if ((unsigned long long)chunk_size > remaining) {
                chunk_size = (size_t)remaining;
            }

            bytes_read = pread(dump_fd,
                               buffer,
                               chunk_size,
                               (off_t)(region->dump_offset + written_for_region));
            if (bytes_read < 0) {
                region_failed = true;
                entry->outcome_reason = "gagal membaca byte region dari mem.dump";
                break;
            }
            if ((size_t)bytes_read != chunk_size) {
                region_failed = true;
                entry->outcome_reason = "isi mem.dump lebih pendek dari metadata region";
                break;
            }

            bytes_written = pwrite(target_mem_fd,
                                   buffer,
                                   chunk_size,
                                   (off_t)(region->start_address + written_for_region));
            if (bytes_written < 0) {
                region_failed = true;
                entry->outcome_reason = "gagal menulis byte region ke target restore";
                break;
            }
            if ((size_t)bytes_written != chunk_size) {
                region_failed = true;
                entry->outcome_reason = "byte yang ditulis ke target restore tidak lengkap";
                break;
            }

            written_for_region += (unsigned long long)bytes_written;
        }

        entry->bytes_written = written_for_region;
        plan->memory_written_bytes += written_for_region;

        if (region_failed) {
            ++plan->memory_failed_regions;
            continue;
        }

        entry->write_succeeded = true;
        if (entry->writeback_kind == MC_WRITEBACK_SAFE) {
            entry->outcome_reason = "berhasil ditulis kembali sebagai kandidat aman";
        } else if (entry->writeback_kind == MC_WRITEBACK_STACK) {
            entry->outcome_reason = "stack berhasil ditulis kembali";
        } else {
            entry->outcome_reason = "berhasil ditulis kembali sebagai kandidat tambahan";
        }
        ++plan->memory_written_regions;
    }

    if (close(target_mem_fd) != 0) {
        close(dump_fd);
        mc_log_system_error("Gagal menutup memori target restore");
        return -1;
    }

    if (close(dump_fd) != 0) {
        mc_log_system_error("Gagal menutup mem.dump");
        return -1;
    }

    plan->memory_skipped_regions = plan->mapping_candidate_regions - plan->memory_attempted_regions;
    return 0;
}

/*
 * Detail singkat per region membantu menunjukkan region mana yang benar-benar
 * dicoba untuk langkah write-back awal, mana yang berhasil, dan mana yang
 * sengaja dilewati karena belum aman untuk dicoba sekarang.
 */
static void mc_print_memory_writeback_details(const mc_restore_plan *plan)
{
    bool has_details = false;

    for (size_t i = 0; i < plan->region_count; ++i) {
        const mc_restore_region *region = &plan->regions[i];
        const mc_restore_mapping_entry *entry = &plan->mapping_entries[i];

        if (entry->kind != MC_MAP_CANDIDATE && entry->writeback_kind != MC_WRITEBACK_FILE_EXEC) {
            continue;
        }

        if (!has_details) {
            puts("Detail write-back  :");
            has_details = true;
        }

        if (entry->write_succeeded) {
            const char *jenis = entry->writeback_kind == MC_WRITEBACK_SAFE ? "aman" :
                                entry->writeback_kind == MC_WRITEBACK_STACK ? "stack" :
                                "tambahan";

            printf("  region_%zu         berhasil (%s, %s 0x%llx-0x%llx)\n",
                   i,
                   jenis,
                   region->label[0] != '\0' ? region->label : "(tanpa label)",
                   region->start_address,
                   region->end_address);
        } else if (entry->writeback_kind == MC_WRITEBACK_FILE_EXEC && entry->writeback_candidate) {
            printf("  region_%zu         siap dari file asli (%s, %s 0x%llx-0x%llx)\n",
                   i,
                   entry->outcome_reason != NULL ? entry->outcome_reason : "file-backed",
                   region->label[0] != '\0' ? region->label : "(tanpa label)",
                   region->start_address,
                   region->end_address);
        } else if (entry->write_attempted) {
            printf("  region_%zu         gagal (%s, %s 0x%llx-0x%llx)\n",
                   i,
                   entry->outcome_reason != NULL ? entry->outcome_reason : "alasan tidak tersedia",
                   region->label[0] != '\0' ? region->label : "(tanpa label)",
                   region->start_address,
                   region->end_address);
        } else {
            printf("  region_%zu         dilewati (%s, %s 0x%llx-0x%llx)\n",
                   i,
                   entry->outcome_reason != NULL ? entry->outcome_reason : "alasan tidak tersedia",
                   region->label[0] != '\0' ? region->label : "(tanpa label)",
                   region->start_address,
                   region->end_address);
        }
    }
}

/*
 * Ringkasan diagnosis ini membantu menjelaskan kenapa target gagal lanjut
 * berjalan setelah resume eksperimen. Fokusnya adalah mengaitkan alamat
 * runtime dengan region checkpoint yang berhasil atau belum berhasil dipulihkan.
 */
static void mc_print_resume_diagnostics(const mc_restore_plan *plan)
{
    const mc_restore_region *region = NULL;

    if (!plan->diagnostics.available) {
        return;
    }

    puts("Diagnostik resume :");

    if (plan->diagnostics.has_runtime_regs) {
        printf("  RIP saat stop    : 0x%llx\n", plan->diagnostics.rip);
        if (plan->diagnostics.rip_region_index >= 0) {
            region = &plan->regions[plan->diagnostics.rip_region_index];
            printf("  Region RIP       : %s (%s)\n",
                   region->label[0] != '\0' ? region->label : "(tanpa label)",
                   mc_describe_region_restore_state(plan, plan->diagnostics.rip_region_index));
        } else {
            puts("  Region RIP       : tidak ditemukan di metadata checkpoint");
        }

        printf("  RSP saat stop    : 0x%llx\n", plan->diagnostics.rsp);
        if (plan->diagnostics.rsp_region_index >= 0) {
            region = &plan->regions[plan->diagnostics.rsp_region_index];
            printf("  Region RSP       : %s (%s)\n",
                   region->label[0] != '\0' ? region->label : "(tanpa label)",
                   mc_describe_region_restore_state(plan, plan->diagnostics.rsp_region_index));
        } else {
            puts("  Region RSP       : tidak ditemukan di metadata checkpoint");
        }

        printf("  RBP saat stop    : 0x%llx\n", plan->diagnostics.rbp);
        if (plan->diagnostics.rbp_region_index >= 0) {
            region = &plan->regions[plan->diagnostics.rbp_region_index];
            printf("  Region RBP       : %s (%s)\n",
                   region->label[0] != '\0' ? region->label : "(tanpa label)",
                   mc_describe_region_restore_state(plan, plan->diagnostics.rbp_region_index));
        } else {
            puts("  Region RBP       : tidak ditemukan di metadata checkpoint");
        }
    } else {
        puts("  Register runtime : belum berhasil dibaca setelah target berhenti lagi");
    }

    if (plan->diagnostics.has_fault_address) {
        printf("  Alamat fault     : 0x%llx\n", plan->diagnostics.fault_address);
        if (plan->diagnostics.fault_region_index >= 0) {
            region = &plan->regions[plan->diagnostics.fault_region_index];
            printf("  Region fault     : %s (%s)\n",
                   region->label[0] != '\0' ? region->label : "(tanpa label)",
                   mc_describe_region_restore_state(plan, plan->diagnostics.fault_region_index));
        } else {
            puts("  Region fault     : tidak ditemukan di metadata checkpoint");
        }
    }

    if (plan->diagnostics.stack_region_found) {
        if (plan->diagnostics.stack_region_restored) {
            puts("  Status stack     : region stack sudah ditulis kembali");
        } else if (plan->diagnostics.stack_region_attempted) {
            puts("  Status stack     : region stack sempat dicoba tetapi belum berhasil");
        } else {
            puts("  Status stack     : region stack masih belum dipulihkan");
        }
    } else {
        puts("  Status stack     : region stack tidak ditemukan di metadata checkpoint");
    }

    printf("  Dugaan utama     : %s\n",
           plan->diagnostics.likely_cause[0] != '\0' ? plan->diagnostics.likely_cause : "(belum ada)");
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
    printf("Kandidat mapping  : %zu\n", plan->mapping_candidate_regions);
    printf("Region berisiko   : %zu\n", plan->mapping_risky_regions);
    printf("Region dilewati   : %zu\n", plan->mapping_skipped_regions);
    printf("Byte kandidat     : %llu\n", plan->mapping_candidate_bytes);
    printf("Kandidat aman     : %zu\n", plan->memory_safe_candidate_regions);
    printf("Kandidat tambahan : %zu\n", plan->memory_extended_candidate_regions);
    printf("Kandidat stack    : %zu\n", plan->memory_stack_candidate_regions);
    printf("File-eksekusi siap: %zu dari %zu\n",
           plan->file_mapping_ready_regions,
           plan->file_mapping_attempted_regions);
    printf("Write-back dicoba : %zu\n", plan->memory_attempted_regions);
    printf("Write-back berhasil: %zu\n", plan->memory_written_regions);
    printf("Write-back gagal  : %zu\n", plan->memory_failed_regions);
    printf("Write-back lewat  : %zu\n", plan->memory_skipped_regions);
    printf("Byte ditulis balik: %llu\n", plan->memory_written_bytes);
    printf("Register dimuat   : %s\n", plan->regs.loaded ? "ya" : "tidak");

    if (plan->regs.loaded) {
        printf("RIP checkpoint    : 0x%llx\n", plan->regs.rip);
        printf("RSP checkpoint    : 0x%llx\n", plan->regs.rsp);
        printf("RBP checkpoint    : 0x%llx\n", plan->regs.rbp);
        printf("RAX checkpoint    : 0x%llx\n", plan->regs.rax);
    }

    printf("Target restore    : %s\n", plan->target.created ? "berhasil dibuat" : "belum dibuat");
    if (plan->target.created) {
        printf("PID target baru   : %d\n", plan->target.pid);
        printf("Status target     : %s\n", plan->target.stopped ? "berhenti dan siap untuk tahap berikutnya" : "belum berhenti");
        printf("Register dicoba   : %s\n", plan->target.regs_apply_attempted ? "ya" : "tidak");
        printf("Register diterapkan: %s\n", plan->target.regs_apply_succeeded ? "berhasil" : "belum berhasil");
    }
    printf("Resume dicoba     : %s\n", plan->target.resume_attempted ? "ya" : "tidak");
    if (plan->target.resume_attempted) {
        if (plan->target.resume_signaled) {
            printf("Hasil resume      : crash oleh sinyal %d\n", plan->target.resume_signal);
        } else if (plan->target.resume_exited) {
            printf("Hasil resume      : keluar dengan kode %d\n", plan->target.resume_exit_code);
        } else if (plan->target.resume_running_briefly && plan->target.resume_stopped_again) {
            puts("Hasil resume      : sempat berjalan singkat lalu dihentikan kembali");
        } else if (plan->target.resume_stopped_again) {
            printf("Hasil resume      : berhenti lagi oleh sinyal %d\n", plan->target.resume_signal);
        } else if (plan->target.resume_completed) {
            puts("Hasil resume      : selesai diamati, tetapi hasilnya tidak khas");
        } else {
            puts("Hasil resume      : percobaan belum selesai diamati");
        }

        printf("Catatan resume    : %s\n",
               plan->target.resume_note[0] != '\0' ? plan->target.resume_note : "(tidak ada)");
    }

    mc_print_resume_diagnostics(plan);

    mc_print_memory_writeback_details(plan);

    puts("Status restore    : metadata dan target restore awal sudah disiapkan untuk tahap berikutnya.");
    puts("Catatan           : resume hanya eksperimen terkontrol setelah restore parsial, bukan bukti restore penuh.");
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

    /*
     * Data register dimuat ke struktur internal agar tahap berikutnya nanti
     * sudah mengetahui konteks eksekusi proses, bukan hanya isi memorinya.
     */
    if (mc_load_register_dump(regs_path, &plan) != 0) {
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

    /*
     * Setelah metadata memori valid, tool mengubahnya menjadi rencana mapping
     * sederhana. Tujuannya adalah mengetahui region mana yang paling realistis
     * untuk dipetakan kembali sebelum benar-benar menyentuh `mmap`.
     */
    if (mc_build_restore_mapping_plan(&plan) != 0) {
        free(plan.regions);
        return 1;
    }

    /*
     * Parent menyiapkan jendela alamat anonim untuk subset region yang paling
     * aman. Jendela ini akan diwariskan ke child restore saat `fork`.
     */
    if (mc_prepare_parent_restore_windows(&plan) != 0) {
        free(plan.mapping_entries);
        free(plan.regions);
        return 1;
    }

    /*
     * Setelah alamat aman disiapkan, tool membuat child baru yang nantinya
     * dipakai sebagai wadah restore. Child berhenti lebih dulu agar parent
     * bisa menulis register dan byte memori secara terkontrol.
     */
    if (mc_create_restore_target(&plan) != 0) {
        free(plan.mapping_entries);
        free(plan.regions);
        return 1;
    }

    /*
     * Setelah child skeleton tersedia, tool mencoba menerapkan register
     * checkpoint ke target tersebut sebagai langkah awal menuju restore nyata.
     */
    if (mc_apply_checkpoint_registers(&plan) != 0) {
        mc_cleanup_restore_target(&plan, false);
        free(plan.mapping_entries);
        free(plan.regions);
        return 1;
    }

    /*
     * Byte dari `mem.dump` sekarang dicoba ditulis kembali ke subset region
     * yang paling aman. Tahap ini masih parsial dan tidak berarti seluruh
     * address space target sudah sama dengan checkpoint.
     */
    if (mc_write_memory_back_to_restore_target(mem_dump_path, &plan) != 0) {
        mc_cleanup_restore_target(&plan, false);
        free(plan.mapping_entries);
        free(plan.regions);
        return 1;
    }

    /*
     * Setelah register dan sebagian memori ditulis, tool mencoba melanjutkan
     * target sebentar untuk melihat reaksi awalnya. Hasil eksperimen ini hanya
     * diamati dan dilaporkan; bukan bukti bahwa restore sudah selesai.
     */
    if (mc_run_controlled_resume_experiment(&plan) != 0) {
        mc_cleanup_restore_target(&plan, false);
        free(plan.mapping_entries);
        free(plan.regions);
        return 1;
    }

    snprintf(ctx->last_checkpoint_dir, sizeof(ctx->last_checkpoint_dir), "%s", checkpoint_dir);

    puts("Checkpoint berhasil dimuat untuk persiapan restore.");
    mc_print_restore_plan_summary(&plan);

    /*
     * Persiapan restore berhenti di sini. Struktur internal sudah cukup untuk
     * tahap berikutnya, tetapi tool belum menulis register atau memori ke
     * target restore yang baru dibuat.
     */
    if (mc_cleanup_restore_target(&plan, true) != 0) {
        free(plan.mapping_entries);
        free(plan.regions);
        return 1;
    }

    free(plan.mapping_entries);
    free(plan.regions);
    return 0;
}
