#include "mini_criu.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

void mc_init_context(mc_context *ctx)
{
    ctx->target_pid = -1;
    ctx->running = false;
    snprintf(ctx->checkpoint_root, sizeof(ctx->checkpoint_root), "%s", MC_DEFAULT_CHECKPOINT_ROOT);
    ctx->last_checkpoint_dir[0] = '\0';
}

void mc_trim_newline(char *line)
{
    size_t len = strlen(line);

    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
        line[--len] = '\0';
    }
}

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

bool mc_directory_exists(const char *path)
{
    struct stat st;

    if (stat(path, &st) != 0) {
        return false;
    }

    return S_ISDIR(st.st_mode);
}

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

int mc_join_path(char *buffer, size_t size, const char *left, const char *right)
{
    int written = snprintf(buffer, size, "%s/%s", left, right);

    if (written < 0 || (size_t)written >= size) {
        return -1;
    }

    return 0;
}

void mc_format_timestamp(char *buffer, size_t size)
{
    time_t now = time(NULL);
    struct tm tm_now;

    localtime_r(&now, &tm_now);
    strftime(buffer, size, "%Y%m%d-%H%M%S", &tm_now);
}

void mc_log_info(const char *message)
{
    fprintf(stdout, "[info] %s\n", message);
}

void mc_log_error(const char *message)
{
    fprintf(stderr, "[galat] %s\n", message);
}

int mc_set_target(mc_context *ctx, pid_t pid)
{
    if (!mc_process_exists(pid)) {
        mc_log_error("PID target tidak berjalan atau tidak terlihat dari lingkungan ini.");
        return 1;
    }

    ctx->target_pid = pid;
    printf("PID target diatur ke %d.\n", pid);
    return 0;
}
