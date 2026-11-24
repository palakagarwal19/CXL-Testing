#ifndef CXL_TEST_COMMON_H
#define CXL_TEST_COMMON_H

#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/cxl_mem.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define CXL_DEFAULT_DEVICE "/dev/cxl/mem0"
#define CXL_MAX_TEST_PAYLOAD 4096U
#define CXL_CMD_FLAG_ENABLED  0x1U
#define CXL_CMD_FLAG_EXCLUSIVE 0x2U

typedef struct {
    const char *test_name;
    const char *log_name;
    const char *report_name;
    const char *device;
    FILE *log;
    int fd;
    bool query_available;
    bool command_present[CXL_MEM_COMMAND_ID_MAX];
    bool command_enabled[CXL_MEM_COMMAND_ID_MAX];
    bool command_exclusive[CXL_MEM_COMMAND_ID_MAX];
} cxl_test_ctx;

static inline void cxl_log(cxl_test_ctx *ctx, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    if (ctx && ctx->log) {
        va_start(ap, fmt);
        vfprintf(ctx->log, fmt, ap);
        va_end(ap);
        fflush(ctx->log);
    }
}

static inline uint16_t cxl_get_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t cxl_get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline uint64_t cxl_get_le64(const uint8_t *p)
{
    return (uint64_t)cxl_get_le32(p) | ((uint64_t)cxl_get_le32(p + 4) << 32);
}

static inline void cxl_put_le32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static inline void cxl_dump(cxl_test_ctx *ctx, const char *label,
                     const uint8_t *buf, size_t length)
{
    size_t i;
    cxl_log(ctx, "%s (%zu bytes):", label, length);
    for (i = 0; i < length; i++) {
        if (i % 16 == 0)
            cxl_log(ctx, "\n  ");
        cxl_log(ctx, "%02x ", buf[i]);
    }
    cxl_log(ctx, "\n");
}

static inline int cxl_query_commands(cxl_test_ctx *ctx)
{
    struct cxl_mem_query_commands first = { .n_commands = 0, .rsvd = 0 };
    struct cxl_mem_query_commands *query;
    size_t bytes;
    uint32_t count, i;

    if (ioctl(ctx->fd, CXL_MEM_QUERY_COMMANDS, &first) < 0) {
        cxl_log(ctx, "[WARN] QUERY_COMMANDS unavailable: %s; attempting the requested command anyway.\n",
                strerror(errno));
        return -1;
    }
    count = first.n_commands;
    if (count > 1024U) {
        cxl_log(ctx, "[WARN] Unexpected command count %u; refusing oversized query.\n", count);
        return -1;
    }
    bytes = sizeof(*query) + (size_t)count * sizeof(struct cxl_command_info);
    query = calloc(1, bytes);
    if (!query) {
        cxl_log(ctx, "[WARN] Cannot allocate command query buffer: %s\n", strerror(errno));
        return -1;
    }
    query->n_commands = count;
    if (ioctl(ctx->fd, CXL_MEM_QUERY_COMMANDS, query) < 0) {
        cxl_log(ctx, "[WARN] QUERY_COMMANDS list failed: %s; attempting command anyway.\n",
                strerror(errno));
        free(query);
        return -1;
    }

    ctx->query_available = true;
    for (i = 0; i < query->n_commands && i < count; i++) {
        uint32_t id = query->commands[i].id;
        if (id >= CXL_MEM_COMMAND_ID_MAX)
            continue;
        ctx->command_present[id] = true;
        ctx->command_enabled[id] =
            (query->commands[i].flags & CXL_CMD_FLAG_ENABLED) != 0;
        ctx->command_exclusive[id] =
            (query->commands[i].flags & CXL_CMD_FLAG_EXCLUSIVE) != 0;
    }
    cxl_log(ctx, "[INFO] Kernel reports %u supported CXL ioctl commands.\n", query->n_commands);
    free(query);
    return 0;
}

/* Returns 1 if usable, 0 if unavailable/disabled, and -1 if query was unavailable. */
static inline int cxl_require_command(cxl_test_ctx *ctx, uint32_t id)
{
    if (!ctx->query_available)
        return -1;
    if (id >= CXL_MEM_COMMAND_ID_MAX || !ctx->command_present[id] ||
        !ctx->command_enabled[id]) {
        cxl_log(ctx, "[SKIP] %s is not enabled for this device/kernel.\n",
                id < CXL_MEM_COMMAND_ID_MAX ? cxl_command_names[id].name : "Unknown command");
        return 0;
    }
    if (ctx->command_exclusive[id]) {
        cxl_log(ctx, "[SKIP] %s is marked exclusive by the kernel.\n",
                cxl_command_names[id].name);
        return 0;
    }
    return 1;
}

static inline int cxl_send(cxl_test_ctx *ctx, uint32_t command_id, uint16_t opcode,
                    const uint8_t *input, uint32_t input_size,
                    uint8_t *output, uint32_t output_capacity,
                    uint32_t *output_size)
{
    struct cxl_send_command cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.id = command_id;
    if (command_id == CXL_MEM_COMMAND_ID_RAW)
        cmd.raw.opcode = opcode;
    cmd.in.size = input_size;
    cmd.in.payload = (uint64_t)(uintptr_t)input;
    cmd.out.size = output_capacity;
    cmd.out.payload = (uint64_t)(uintptr_t)output;

    cxl_log(ctx, "[INFO] ioctl id=%u opcode=0x%04x input=%u output_capacity=%u\n",
            command_id, opcode, input_size, output_capacity);
    if (input_size && input)
        cxl_dump(ctx, "Request", input, input_size);

    if (ioctl(ctx->fd, CXL_MEM_SEND_COMMAND, &cmd) < 0) {
        cxl_log(ctx, "[ERROR] CXL_MEM_SEND_COMMAND: %s (errno=%d)\n",
                strerror(errno), errno);
        return -1;
    }
    cxl_log(ctx, "[INFO] ioctl succeeded; mailbox retval=0x%x; response=%u bytes\n",
            cmd.retval, cmd.out.size);
    if (cmd.out.size > output_capacity) {
        cxl_log(ctx, "[ERROR] Device response exceeds output buffer.\n");
        return -1;
    }
    if (output_size)
        *output_size = cmd.out.size;
    if (cmd.retval != 0) {
        cxl_log(ctx, "[ERROR] Device returned non-zero mailbox retval 0x%x.\n", cmd.retval);
        return -2;
    }
    if (cmd.out.size && output)
        cxl_dump(ctx, "Response", output, cmd.out.size);
    return 0;
}

static inline int cxl_test_begin(cxl_test_ctx *ctx, const char *test_name,
                          const char *log_name, const char *report_name,
                          const char *device)
{
    time_t now = time(NULL);
    memset(ctx, 0, sizeof(*ctx));
    ctx->test_name = test_name;
    ctx->log_name = log_name;
    ctx->report_name = report_name;
    ctx->device = device ? device : CXL_DEFAULT_DEVICE;
    ctx->fd = -1;
    ctx->log = fopen(log_name, "w");
    if (!ctx->log) {
        fprintf(stderr, "Cannot create %s: %s\n", log_name, strerror(errno));
        return -1;
    }
    cxl_log(ctx, "========================================\n");
    cxl_log(ctx, "CXL CCI INDIVIDUAL TEST: %s\n", test_name);
    cxl_log(ctx, "Device: %s\n", ctx->device);
    cxl_log(ctx, "Started: %s", ctime(&now));

    ctx->fd = open(ctx->device, O_RDWR | O_CLOEXEC);
    if (ctx->fd < 0) {
        cxl_log(ctx, "[FAIL] Cannot open device: %s\n", strerror(errno));
        return -1;
    }
    (void)cxl_query_commands(ctx);
    return 0;
}

static inline int cxl_test_end(cxl_test_ctx *ctx, const char *status, const char *detail)
{
    FILE *report;
    int exit_code = 1;
    time_t now = time(NULL);
    if (ctx->fd >= 0)
        close(ctx->fd);
    cxl_log(ctx, "\nRESULT: %s\nDETAIL: %s\n", status, detail ? detail : "");
    report = fopen(ctx->report_name, "w");
    if (report) {
        fprintf(report, "CXL CCI TEST RESULT\n===================\n");
        fprintf(report, "Test: %s\nDevice: %s\nDate: %s",
                ctx->test_name, ctx->device, ctime(&now));
        fprintf(report, "Result: %s\nDetail: %s\nLog: %s\n",
                status, detail ? detail : "", ctx->log_name);
        fclose(report);
    } else {
        cxl_log(ctx, "[WARN] Could not write %s: %s\n", ctx->report_name, strerror(errno));
    }
    if (ctx->log)
        fclose(ctx->log);
    if (strcmp(status, "PASS") == 0)
        exit_code = 0;
    else if (strcmp(status, "SKIP") == 0)
        exit_code = 77;
    return exit_code;
}

static inline const char *cxl_device_arg(int argc, char **argv, int *first_option)
{
    if (argc > 1 && argv[1][0] != '-') {
        *first_option = 2;
        return argv[1];
    }
    *first_option = 1;
    return CXL_DEFAULT_DEVICE;
}

static inline int cxl_parse_u32(const char *s, uint32_t *value)
{
    char *end = NULL;
    unsigned long long n;
    errno = 0;
    n = strtoull(s, &end, 0);
    if (errno || !end || *end || n > UINT32_MAX)
        return -1;
    *value = (uint32_t)n;
    return 0;
}

#endif
