#include "mini_criu.h"

#include <stdio.h>
#include <string.h>

/*
 * Menerjemahkan nama command dari input pengguna ke enum internal.
 */
static mc_command_kind mc_lookup_command(const char *name)
{
    if (strcmp(name, "help") == 0) {
        return MC_CMD_HELP;
    }
    if (strcmp(name, "status") == 0) {
        return MC_CMD_STATUS;
    }
    if (strcmp(name, "clear") == 0 || strcmp(name, "/clear") == 0) {
        return MC_CMD_CLEAR;
    }
    if (strcmp(name, "set-target") == 0) {
        return MC_CMD_SET_TARGET;
    }
    if (strcmp(name, "freeze") == 0) {
        return MC_CMD_FREEZE;
    }
    if (strcmp(name, "dump-memory") == 0) {
        return MC_CMD_DUMP_MEMORY;
    }
    if (strcmp(name, "restore") == 0) {
        return MC_CMD_RESTORE;
    }
    if (strcmp(name, "exit") == 0 || strcmp(name, "quit") == 0) {
        return MC_CMD_EXIT;
    }

    return MC_CMD_INVALID;
}

/*
 * Memvalidasi jumlah argumen untuk setiap command.
 *
 * Fungsi ini dipakai baik untuk mode satu command maupun mode interaktif.
 */
static int mc_parse_tokens(int argc, char **argv, mc_command *cmd)
{
    if (argc <= 0) {
        cmd->kind = MC_CMD_INVALID;
        cmd->argc = 0;
        cmd->error = NULL;
        return 1;
    }

    if (argc > MC_MAX_TOKENS) {
        cmd->kind = MC_CMD_INVALID;
        cmd->argc = argc;
        cmd->error = "terlalu banyak argumen";
        return -1;
    }

    cmd->kind = mc_lookup_command(argv[0]);
    cmd->argc = argc;
    cmd->error = NULL;

    for (int i = 0; i < argc; ++i) {
        cmd->argv[i] = argv[i];
    }

    if (cmd->kind == MC_CMD_INVALID) {
        cmd->error = "perintah tidak dikenali";
        return -1;
    }

    switch (cmd->kind) {
    case MC_CMD_HELP:
    case MC_CMD_STATUS:
    case MC_CMD_CLEAR:
    case MC_CMD_FREEZE:
    case MC_CMD_DUMP_MEMORY:
    case MC_CMD_EXIT:
        if (argc != 1) {
            cmd->error = "perintah ini tidak menerima argumen tambahan";
            return -1;
        }
        break;
    case MC_CMD_SET_TARGET:
        if (argc != 2) {
            cmd->error = "penggunaan: set-target <pid>";
            return -1;
        }
        break;
    case MC_CMD_RESTORE:
        if (argc != 2) {
            cmd->error = "penggunaan: restore <checkpoint_dir>";
            return -1;
        }
        break;
    case MC_CMD_INVALID:
        cmd->error = "perintah tidak dikenali";
        return -1;
    }

    return 0;
}

/*
 * Menampilkan identitas singkat aplikasi saat CLI dimulai.
 */
void mc_print_banner(void)
{
    puts("mini-criu");
    puts("Scaffold prototipe akademik checkpoint/restore untuk proses Linux.");
    puts("Ketik 'help' untuk melihat perintah yang tersedia.");
}

/*
 * Menampilkan daftar command yang bisa dipakai pengguna.
 */
void mc_print_help(void)
{
    puts("");
    puts("Perintah yang tersedia:");
    puts("  help                      Menampilkan pesan bantuan ini");
    puts("  status                    Menampilkan status CLI saat ini");
    puts("  clear, /clear             Membersihkan tampilan terminal");
    puts("  set-target <pid>          Memilih proses untuk diperiksa/checkpoint");
    puts("  freeze                    Memulai snapshot, menyimpan register, dan menahan target tetap stop");
    puts("  dump-memory               Menyelesaikan snapshot aktif dengan mem.meta dan mem.dump");
    puts("  restore <checkpoint_dir>  Memuat checkpoint, membuat target restore, dan mencoba register serta write-back memori awal");
    puts("  exit                      Keluar dari shell interaktif");
    puts("");
}

/*
 * Menampilkan konteks sesi saat ini agar pengguna tahu target dan folder
 * checkpoint yang sedang aktif.
 */
void mc_print_status(const mc_context *ctx)
{
    puts("");
    puts("status mini-criu");
    puts("-----------------");

    if (ctx->target_pid > 0) {
        printf("PID target        : %d\n", ctx->target_pid);
        printf("Target tersedia   : %s\n", mc_process_exists(ctx->target_pid) ? "ya" : "tidak");
    } else {
        puts("PID target        : belum diatur");
        puts("Target tersedia   : n/a");
    }

    printf("Root checkpoint   : %s\n", ctx->checkpoint_root);
    printf("Checkpoint akhir  : %s\n",
           ctx->last_checkpoint_dir[0] != '\0' ? ctx->last_checkpoint_dir : "(belum ada)");
    printf("Snapshot aktif    : %s\n", ctx->snapshot_active ? "ya" : "tidak");
    printf("ID snapshot       : %s\n",
           ctx->snapshot_active && ctx->active_snapshot_id[0] != '\0' ? ctx->active_snapshot_id : "(tidak ada)");
    puts("");
}

/*
 * Memecah satu baris input menjadi token command dan argumen.
 */
int mc_parse_command(char *line, mc_command *cmd)
{
    char *tokens[MC_MAX_TOKENS];
    int argc = 0;

    mc_trim_newline(line);
    line = mc_trim_whitespace(line);

    if (*line == '\0') {
        cmd->kind = MC_CMD_INVALID;
        cmd->argc = 0;
        cmd->error = NULL;
        return 1;
    }

    char *token = strtok(line, " \t");
    while (token != NULL) {
        if (argc >= MC_MAX_TOKENS) {
            cmd->kind = MC_CMD_INVALID;
            cmd->argc = argc + 1;
            cmd->error = "terlalu banyak argumen";
            return -1;
        }

        tokens[argc++] = token;
        token = strtok(NULL, " \t");
    }

    return mc_parse_tokens(argc, tokens, cmd);
}

/*
 * Menjalankan command yang sudah lolos parsing.
 */
int mc_execute_command(mc_context *ctx, const mc_command *cmd)
{
    pid_t pid = -1;

    switch (cmd->kind) {
    case MC_CMD_HELP:
        mc_print_help();
        return 0;
    case MC_CMD_STATUS:
        mc_print_status(ctx);
        return 0;
    case MC_CMD_CLEAR:
        /* Gunakan escape ANSI agar layar terminal dibersihkan dan kursor kembali ke atas. */
        fputs("\033[2J\033[H", stdout);
        fflush(stdout);
        return 0;
    case MC_CMD_SET_TARGET:
        if (!mc_parse_pid(cmd->argv[1], &pid)) {
            mc_log_error("PID tidak valid. Gunakan bilangan bulat positif.");
            return 1;
        }
        return mc_set_target(ctx, pid);
    case MC_CMD_FREEZE:
        return mc_freeze_target(ctx);
    case MC_CMD_DUMP_MEMORY:
        return mc_dump_memory(ctx);
    case MC_CMD_RESTORE:
        return mc_restore_checkpoint(ctx, cmd->argv[1]);
    case MC_CMD_EXIT:
        ctx->running = false;
        puts("Keluar dari mini-criu.");
        return 0;
    case MC_CMD_INVALID:
        mc_log_error("Perintah tidak dikenali.");
        return 1;
    }

    return 1;
}

/*
 * Menjalankan CLI untuk satu command langsung dari argumen program.
 */
static int mc_run_single_command(mc_context *ctx, int argc, char **argv)
{
    mc_command cmd;
    int parse_status = mc_parse_tokens(argc, argv, &cmd);

    if (parse_status == 1) {
        return 0;
    }

    if (parse_status < 0) {
        mc_log_error(cmd.error != NULL ? cmd.error : "gagal memproses perintah");
        return 1;
    }

    return mc_execute_command(ctx, &cmd);
}

/*
 * Menjalankan mode REPL interaktif.
 *
 * Loop ini terus membaca command sampai pengguna menjalankan `exit` atau input
 * berakhir.
 */
static int mc_run_repl(mc_context *ctx)
{
    char line[512];

    ctx->running = true;
    while (ctx->running) {
        mc_command cmd;
        int parse_status;

        printf("mini-criu> ");
        fflush(stdout);

        if (fgets(line, sizeof(line), stdin) == NULL) {
            puts("");
            break;
        }

        parse_status = mc_parse_command(line, &cmd);
        if (parse_status == 1) {
            continue;
        }

        if (parse_status < 0) {
            mc_log_error(cmd.error != NULL ? cmd.error : "gagal memproses perintah");
            continue;
        }

        mc_execute_command(ctx, &cmd);
    }

    return 0;
}

/*
 * Menentukan apakah program berjalan dalam mode satu command atau REPL.
 */
int mc_run_cli(mc_context *ctx, int argc, char **argv)
{
    if (argc > 1) {
        return mc_run_single_command(ctx, argc - 1, argv + 1);
    }

    return mc_run_repl(ctx);
}
