#ifndef MINI_CRIU_H
#define MINI_CRIU_H

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define MC_MAX_TOKENS 8
#define MC_DEFAULT_CHECKPOINT_ROOT "checkpoints"
#define MC_REGION_PERMS_LEN 5
#define MC_REGION_LABEL_LEN 256

typedef enum {
    MC_CMD_INVALID = -1,
    MC_CMD_HELP = 0,
    MC_CMD_STATUS,
    MC_CMD_CLEAR,
    MC_CMD_SET_TARGET,
    MC_CMD_FREEZE,
    MC_CMD_DUMP_MEMORY,
    MC_CMD_RESTORE,
    MC_CMD_EXIT
} mc_command_kind;

typedef struct {
    mc_command_kind kind;
    int argc;
    char *argv[MC_MAX_TOKENS];
    const char *error;
} mc_command;

typedef struct {
    pid_t target_pid;
    char checkpoint_root[PATH_MAX];
    char last_checkpoint_dir[PATH_MAX];
    bool running;
} mc_context;

typedef struct {
    unsigned long long start_address;
    unsigned long long end_address;
    unsigned long long offset;
    char permissions[MC_REGION_PERMS_LEN];
    char label[MC_REGION_LABEL_LEN];
    bool selected;
} mc_memory_region;

void mc_init_context(mc_context *ctx);
int mc_run_cli(mc_context *ctx, int argc, char **argv);
void mc_print_banner(void);
void mc_print_help(void);
void mc_print_status(const mc_context *ctx);
int mc_parse_command(char *line, mc_command *cmd);
int mc_execute_command(mc_context *ctx, const mc_command *cmd);

void mc_trim_newline(char *line);
char *mc_trim_whitespace(char *text);
bool mc_parse_pid(const char *text, pid_t *pid_out);
bool mc_process_exists(pid_t pid);
bool mc_directory_exists(const char *path);
int mc_ensure_directory(const char *path);
int mc_write_text_file(const char *path, const char *contents);
int mc_join_path(char *buffer, size_t size, const char *left, const char *right);
void mc_format_timestamp(char *buffer, size_t size);
void mc_log_info(const char *message);
void mc_log_error(const char *message);
void mc_log_system_error(const char *message);

int mc_set_target(mc_context *ctx, pid_t pid);
int mc_freeze_target(mc_context *ctx);
int mc_dump_memory(mc_context *ctx);
int mc_restore_checkpoint(mc_context *ctx, const char *checkpoint_dir);

#endif
