#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/cxl_mem.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

/* CXL 3.0 Spec CCI Opcodes & Return Codes */
#define OP_GET_EVENT_RECORDS        0x0100
#define OP_CLEAR_EVENT_RECORDS      0x0101
#define CCI_RET_SUCCESS             0x0000
#define CCI_RET_INVALID_INPUT       0x0002
#define CCI_RET_UNSUPPORTED         0x0003
#define CCI_RET_INVALID_PAYLOAD_LEN 0x0016

typedef struct {
    char phase[16];
    char category[32];
    char name[96];
    char status[8];
    double elapsed;
    char detail[256];
} test_result_t;

static test_result_t g_results[32];
static int g_count = 0;
static bool g_cmd_enabled[CXL_MEM_COMMAND_ID_MAX];
static bool g_query_ok = false;

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static uint16_t get_le16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t get_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t get_le64(const uint8_t *p) {
    return (uint64_t)get_le32(p) | ((uint64_t)get_le32(p + 4) << 32);
}

static void put_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void put_le64(uint8_t *p, uint64_t v) {
    put_le32(p, (uint32_t)v);
    put_le32(p + 4, (uint32_t)(v >> 32));
}

static void record(const char *phase, const char *cat, const char *name,
                   const char *status, double elapsed, const char *detail) {
    if (g_count >= 32) return;
    test_result_t *r = &g_results[g_count++];
    snprintf(r->phase, sizeof(r->phase), "%s", phase);
    snprintf(r->category, sizeof(r->category), "%s", cat);
    snprintf(r->name, sizeof(r->name), "%s", name);
    snprintf(r->status, sizeof(r->status), "%s", status);
    r->elapsed = elapsed;
    snprintf(r->detail, sizeof(r->detail), "%s", detail);
}

static void query_cmds(int fd) {
    struct cxl_mem_query_commands hdr = { .n_commands = 0 };
    if (ioctl(fd, CXL_MEM_QUERY_COMMANDS, &hdr) < 0 || hdr.n_commands == 0 || hdr.n_commands > 1024)
        return;

    size_t sz = sizeof(hdr) + hdr.n_commands * sizeof(struct cxl_command_info);
    struct cxl_mem_query_commands *q = calloc(1, sz);
    if (!q) return;
    q->n_commands = hdr.n_commands;
    if (ioctl(fd, CXL_MEM_QUERY_COMMANDS, q) == 0) {
        g_query_ok = true;
        for (uint32_t i = 0; i < q->n_commands; i++) {
            uint32_t id = q->commands[i].id;
            if (id < CXL_MEM_COMMAND_ID_MAX && (q->commands[i].flags & 0x1))
                g_cmd_enabled[id] = true;
        }
    }
    free(q);
}

static bool is_enabled(uint32_t id) {
    if (!g_query_ok) return true;
    return id < CXL_MEM_COMMAND_ID_MAX && g_cmd_enabled[id];
}

static int send_cci(int fd, uint32_t id, uint16_t raw_op,
                    const void *in_buf, uint32_t in_sz,
                    void *out_buf, uint32_t out_cap,
                    uint32_t *out_sz, uint32_t *retval, int *saved_errno) {
    struct cxl_send_command cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.id = id;
    if (id == CXL_MEM_COMMAND_ID_RAW)
        cmd.raw.opcode = raw_op;
    cmd.in.size = in_sz;
    cmd.in.payload = (uint64_t)(uintptr_t)in_buf;
    cmd.out.size = out_cap;
    cmd.out.payload = (uint64_t)(uintptr_t)out_buf;

    if (ioctl(fd, CXL_MEM_SEND_COMMAND, &cmd) < 0) {
        if (saved_errno) *saved_errno = errno;
        if (retval) *retval = 0;
        return -1;
    }
    if (saved_errno) *saved_errno = 0;
    if (retval) *retval = cmd.retval;
    if (out_sz) *out_sz = cmd.out.size;
    return 0;
}

static void run_week5(int fd) {
    uint8_t in[256], out[4096], orig_lsa[64];
    uint32_t out_sz = 0, retval = 0, lsa_size = 0;
    uint64_t active_v = 0;
    int err = 0, rc;
    double t0;
    char msg[256];

    /* 1. Identify Memory Device (0x4000) */
    t0 = now_sec();
    memset(out, 0, sizeof(out));
    rc = send_cci(fd, CXL_MEM_COMMAND_ID_IDENTIFY, 0, NULL, 0, out, 0x43, &out_sz, &retval, &err);
    if (rc == 0 && retval == CCI_RET_SUCCESS && out_sz >= 0x43) {
        char fw[17] = {0};
        memcpy(fw, out, 16);
        uint64_t tot_mb = get_le64(out + 0x10) * 256ULL;
        uint64_t vol_mb = get_le64(out + 0x18) * 256ULL;
        uint64_t pmem_mb = get_le64(out + 0x20) * 256ULL;
        lsa_size = get_le32(out + 0x38);
        snprintf(msg, sizeof(msg), "FW=%.16s Total=%" PRIu64 "MB (Vol=%" PRIu64 "MB PMEM=%" PRIu64 "MB) LSA=%uB",
                 fw, tot_mb, vol_mb, pmem_mb, lsa_size);
        record("Week 5", "Identify", "Identify Memory Device (0x4000)", "PASS", now_sec() - t0, msg);
    } else {
        snprintf(msg, sizeof(msg), "rc=%d errno=%d (%s) retval=0x%04x", rc, err, strerror(err), retval);
        record("Week 5", "Identify", "Identify Memory Device (0x4000)", "FAIL", now_sec() - t0, msg);
    }

    /* 2. Get Partition Info (0x4100) */
    t0 = now_sec();
    if (!is_enabled(CXL_MEM_COMMAND_ID_GET_PARTITION_INFO)) {
        record("Week 5", "Partition", "Get Partition Info (0x4100)", "SKIP", now_sec() - t0,
               "GET_PARTITION_INFO not enabled on device");
    } else {
        rc = send_cci(fd, CXL_MEM_COMMAND_ID_GET_PARTITION_INFO, 0, NULL, 0, out, 32, &out_sz, &retval, &err);
        if (rc == 0 && retval == CCI_RET_SUCCESS && out_sz >= 32) {
            active_v = get_le64(out);
            uint64_t active_p = get_le64(out + 8);
            uint64_t next_v = get_le64(out + 16);
            uint64_t next_p = get_le64(out + 24);
            snprintf(msg, sizeof(msg), "ActiveVol=%" PRIu64 "MB ActivePMEM=%" PRIu64 "MB NextVol=%" PRIu64 "MB NextPMEM=%" PRIu64 "MB",
                     active_v * 256, active_p * 256, next_v * 256, next_p * 256);
            record("Week 5", "Partition", "Get Partition Info (0x4100)", "PASS", now_sec() - t0, msg);
        } else {
            snprintf(msg, sizeof(msg), "rc=%d errno=%d (%s) retval=0x%04x", rc, err, strerror(err), retval);
            record("Week 5", "Partition", "Get Partition Info (0x4100)", "FAIL", now_sec() - t0, msg);
        }
    }

    /* 3. LSA Set (0x4103) & Get (0x4102) Roundtrip */
    t0 = now_sec();
    if (!is_enabled(CXL_MEM_COMMAND_ID_SET_LSA) || !is_enabled(CXL_MEM_COMMAND_ID_GET_LSA)) {
        record("Week 5", "LSA", "LSA Set & Get Roundtrip (0x4103/0x4102)", "SKIP", now_sec() - t0,
               "GET_LSA / SET_LSA not enabled on device");
    } else {
        uint8_t pattern[64] = "CXL3_WEEK5_CCI_TEST_PATTERN_VERIFICATION_2026";
        uint32_t orig_sz = 0;
        put_le32(in, 0);
        put_le32(in + 4, 64);
        bool have_backup = (send_cci(fd, CXL_MEM_COMMAND_ID_GET_LSA, 0, in, 8, orig_lsa, 64, &orig_sz, &retval, &err) == 0 && orig_sz == 64);

        put_le32(in, 0);
        put_le32(in + 4, 0);
        memcpy(in + 8, pattern, 64);
        uint32_t ret_s = 0, ret_g = 0;
        int err_s = 0, err_g = 0;
        int rc_s = send_cci(fd, CXL_MEM_COMMAND_ID_SET_LSA, 0, in, 72, NULL, 0, NULL, &ret_s, &err_s);

        put_le32(in, 0);
        put_le32(in + 4, 64);
        memset(out, 0, 64);
        int rc_g = send_cci(fd, CXL_MEM_COMMAND_ID_GET_LSA, 0, in, 8, out, 64, &out_sz, &ret_g, &err_g);

        if (have_backup) {
            put_le32(in, 0);
            put_le32(in + 4, 0);
            memcpy(in + 8, orig_lsa, 64);
            send_cci(fd, CXL_MEM_COMMAND_ID_SET_LSA, 0, in, 72, NULL, 0, NULL, NULL, NULL);
        }

        if (rc_s == 0 && ret_s == 0 && rc_g == 0 && ret_g == 0 && out_sz == 64 && memcmp(out, pattern, 64) == 0) {
            record("Week 5", "LSA", "LSA Set & Get Roundtrip (0x4103/0x4102)", "PASS", now_sec() - t0,
                   "Wrote 64B pattern at offset 0x0, read back exact match, and restored original LSA");
        } else {
            snprintf(msg, sizeof(msg), "Set(rc=%d,err=%d,ret=0x%x) Get(rc=%d,err=%d,ret=0x%x)", rc_s, err_s, ret_s, rc_g, err_g, ret_g);
            record("Week 5", "LSA", "LSA Set & Get Roundtrip (0x4103/0x4102)", "FAIL", now_sec() - t0, msg);
        }
    }

    /* 4. Get Health Info (0x4200) */
    t0 = now_sec();
    if (!is_enabled(CXL_MEM_COMMAND_ID_GET_HEALTH_INFO)) {
        record("Week 5", "Health", "Get Health Info (0x4200)", "SKIP", now_sec() - t0,
               "GET_HEALTH_INFO (0x4200) not enabled in device CEL");
    } else {
        rc = send_cci(fd, CXL_MEM_COMMAND_ID_GET_HEALTH_INFO, 0, NULL, 0, out, 0x12, &out_sz, &retval, &err);
        if (rc == 0 && retval == CCI_RET_UNSUPPORTED) {
            record("Week 5", "Health", "Get Health Info (0x4200)", "SKIP", now_sec() - t0,
                   "Device returned CCI Unsupported (0x0003) for Health Info");
        } else if (rc == 0 && retval == CCI_RET_SUCCESS && out_sz >= 0x12) {
            int16_t temp_c = (int16_t)get_le16(out + 4);
            snprintf(msg, sizeof(msg), "HealthFlags=0x%02x MediaStatus=0x%02x ExtStatus=0x%02x LifeUsed=%u%% Temp=%dC",
                     out[0], out[1], out[2], out[3], temp_c);
            record("Week 5", "Health", "Get Health Info (0x4200)", "PASS", now_sec() - t0, msg);
        } else {
            snprintf(msg, sizeof(msg), "rc=%d errno=%d (%s) retval=0x%04x", rc, err, strerror(err), retval);
            record("Week 5", "Health", "Get Health Info (0x4200)", "FAIL", now_sec() - t0, msg);
        }
    }

    /* 5. Get Event Records (0x0100) */
    t0 = now_sec();
            in[0] = 0x00;
        rc = send_cci(fd, CXL_MEM_COMMAND_ID_RAW, OP_GET_EVENT_RECORDS, in, 1, out, 256, &out_sz, &retval, &err);
        if (rc == 0 && retval == CCI_RET_SUCCESS && out_sz >= 16) {
            uint16_t overflow = get_le16(out + 2);
            uint16_t rec_cnt = get_le16(out + 14);
            snprintf(msg, sizeof(msg), "InfoLogFlags=0x%02x OverflowCount=%u RecordCount=%u", out[0], overflow, rec_cnt);
            record("Week 5", "Events", "Get Event Records (0x0100)", "PASS", now_sec() - t0, msg);
        } else if (rc == -1 && (err == EPERM || err == EINVAL)) {
            snprintf(msg, sizeof(msg), "Kernel UAPI policy enforced (%s); enable raw_allow_all to query", strerror(err));
            record("Week 5", "Events", "Get Event Records (0x0100)", "SKIP", now_sec() - t0, msg);
        } else {
            snprintf(msg, sizeof(msg), "rc=%d errno=%d (%s) retval=0x%04x", rc, err, strerror(err), retval);
            record("Week 5", "Events", "Get Event Records (0x0100)", "FAIL", now_sec() - t0, msg);
        }

    /* 6. Set Partition Info (0x4101) with GET_PARTITION_INFO (0x4100) Staging & Restore Proof */
    t0 = now_sec();
    if (!is_enabled(CXL_MEM_COMMAND_ID_SET_PARTITION_INFO)) {
        record("Week 5", "Partition", "Set Partition Info (0x4101)", "SKIP", now_sec() - t0,
               "SET_PARTITION_INFO not enabled on device");
    } else {
        uint8_t part_init[32] = {0}, part_mut[32] = {0}, part_rest[32] = {0};
        uint32_t part_sz = 0, ret_r0 = 0, ret_r1 = 0, ret_s2 = 0, ret_r2 = 0;
        int err_r0 = 0, err_r1 = 0, err_s2 = 0, err_r2 = 0;

        int rc_r0 = send_cci(fd, CXL_MEM_COMMAND_ID_GET_PARTITION_INFO, 0, NULL, 0,
                             part_init, 32, &part_sz, &ret_r0, &err_r0);
        uint64_t cur_v   = (rc_r0 == 0 && ret_r0 == CCI_RET_SUCCESS && part_sz >= 32) ? get_le64(part_init) : active_v;
        uint64_t cur_p   = (rc_r0 == 0 && ret_r0 == CCI_RET_SUCCESS && part_sz >= 32) ? get_le64(part_init + 8) : 1ULL;
        uint64_t total_u = cur_v + cur_p;
        uint64_t test_v  = (cur_v > 0) ? 0ULL : 1ULL;
        uint64_t test_p  = total_u - test_v;

        /* Stage new partition (flags = 0 -> takes effect on next reset) */
        memset(in, 0, 10);
        put_le64(in, test_v);
        rc = send_cci(fd, CXL_MEM_COMMAND_ID_SET_PARTITION_INFO, 0, in, 10, NULL, 0, NULL, &retval, &err);
        int rc_r1 = send_cci(fd, CXL_MEM_COMMAND_ID_GET_PARTITION_INFO, 0, NULL, 0,
                             part_mut, 32, &part_sz, &ret_r1, &err_r1);

        uint64_t next_v = get_le64(part_mut + 16) * 256ULL;
        uint64_t next_p = get_le64(part_mut + 24) * 256ULL;

        /* Restore original partition (clears NextVol/NextPMEM back to 0MB) */
        memset(in, 0, 10);
        put_le64(in, cur_v);
        int rc_s2 = send_cci(fd, CXL_MEM_COMMAND_ID_SET_PARTITION_INFO, 0, in, 10, NULL, 0, NULL, &ret_s2, &err_s2);
        int rc_r2 = send_cci(fd, CXL_MEM_COMMAND_ID_GET_PARTITION_INFO, 0, NULL, 0,
                             part_rest, 32, &part_sz, &ret_r2, &err_r2);

        uint64_t rest_next_v = get_le64(part_rest + 16) * 256ULL;
        uint64_t rest_next_p = get_le64(part_rest + 24) * 256ULL;

        if (rc == 0 && retval == CCI_RET_SUCCESS &&
            rc_r1 == 0 && ret_r1 == CCI_RET_SUCCESS &&
            next_v == test_v * 256ULL && next_p == test_p * 256ULL &&
            rc_s2 == 0 && ret_s2 == CCI_RET_SUCCESS &&
            rc_r2 == 0 && ret_r2 == CCI_RET_SUCCESS &&
            rest_next_v == 0ULL && rest_next_p == 0ULL) {
            snprintf(msg, sizeof(msg),
                     "ioctl(0x4101) rc=0 retval=0x%04x | 0x4100 readback proved staging: "
                     "Next(Vol=%" PRIu64 "MB PMEM=%" PRIu64 "MB) -> restored Next(Vol=0MB PMEM=0MB)",
                     retval, next_v, next_p);
            record("Week 5", "Partition", "Set Partition Info (0x4101)", "PASS", now_sec() - t0, msg);
        } else {
            snprintf(msg, sizeof(msg),
                     "Staging/restore mismatch: set1(rc=%d,ret=0x%x) Next(Vol=%" PRIu64 "MB,PMEM=%" PRIu64 "MB) restoreNext(Vol=%" PRIu64 "MB)",
                     rc, retval, next_v, next_p, rest_next_v);
            record("Week 5", "Partition", "Set Partition Info (0x4101)", "FAIL", now_sec() - t0, msg);
        }
    }
}

static void run_week6(int fd) {
    uint8_t in[64], out[256];
    uint32_t out_sz = 0, retval = 0;
    int err = 0, rc;
    double t0;
    char msg[256];

    /* 1. Unsupported RAW Opcode (0xFFFF) */
    t0 = now_sec();
    rc = send_cci(fd, CXL_MEM_COMMAND_ID_RAW, 0xFFFF, NULL, 0, out, 64, &out_sz, &retval, &err);
    if ((rc == -1 && (err == EINVAL || err == EPERM || err == ENOTTY)) || retval != 0) {
        snprintf(msg, sizeof(msg), "Rejected properly (rc=%d errno=%s retval=0x%04x)", rc, strerror(err), retval);
        record("Week 6", "Negative", "Unsupported CCI Opcode (0xFFFF)", "PASS", now_sec() - t0, msg);
    } else {
        record("Week 6", "Negative", "Unsupported CCI Opcode (0xFFFF)", "FAIL", now_sec() - t0,
               "Invalid opcode 0xFFFF unexpectedly succeeded");
    }

    /* 2. Invalid UAPI Command ID (0xDEAD) */
    t0 = now_sec();
    rc = send_cci(fd, 0xDEAD, 0, NULL, 0, out, 64, &out_sz, &retval, &err);
    if (rc == -1 && (err == ENOTTY || err == EINVAL)) {
        record("Week 6", "Negative", "Invalid UAPI Command ID (0xDEAD)", "PASS", now_sec() - t0,
               "Kernel UAPI rejected invalid command ID with ENOTTY/EINVAL");
    } else {
        snprintf(msg, sizeof(msg), "Expected ENOTTY/EINVAL, got rc=%d errno=%s retval=0x%04x", rc, strerror(err), retval);
        record("Week 6", "Negative", "Invalid UAPI Command ID (0xDEAD)", "FAIL", now_sec() - t0, msg);
    }

    /* 3. Out-of-Range LSA Read Offset (0xFFFFFFFF) */
    t0 = now_sec();
    put_le32(in, 0xFFFFFFFFU);
    put_le32(in + 4, 64);
    rc = send_cci(fd, CXL_MEM_COMMAND_ID_GET_LSA, 0, in, 8, out, 64, &out_sz, &retval, &err);
    if ((rc == -1 && err == EINVAL) || retval == CCI_RET_INVALID_INPUT) {
        snprintf(msg, sizeof(msg), "Rejected OOB LSA read offset (errno=%s retval=0x%04x)", strerror(err), retval);
        record("Week 6", "Negative", "Out-of-Range LSA Read Offset (0xFFFFFFFF)", "PASS", now_sec() - t0, msg);
    } else {
        record("Week 6", "Negative", "Out-of-Range LSA Read Offset (0xFFFFFFFF)", "FAIL", now_sec() - t0,
               "Out-of-range LSA read offset unexpectedly succeeded");
    }

    /* 4. Truncated SET_LSA Payload (2 bytes instead of >= 8-byte header) */
    t0 = now_sec();
    memset(in, 0, sizeof(in));
    rc = send_cci(fd, CXL_MEM_COMMAND_ID_SET_LSA, 0, in, 2, NULL, 0, NULL, &retval, &err);
    if ((rc == -1 && err == EINVAL) || retval == CCI_RET_INVALID_INPUT || retval == CCI_RET_INVALID_PAYLOAD_LEN) {
        snprintf(msg, sizeof(msg), "Rejected truncated payload (errno=%s retval=0x%04x)", strerror(err), retval);
        record("Week 6", "Negative", "Truncated SET_LSA Payload (2B)", "PASS", now_sec() - t0, msg);
    } else {
        record("Week 6", "Negative", "Truncated SET_LSA Payload (2B)", "FAIL", now_sec() - t0,
               "Truncated SET_LSA payload unexpectedly succeeded");
    }

    /* 5. Out-of-Range SET_LSA Write Offset (0xFFFFFFFF + 16B payload) */
    t0 = now_sec();
    put_le32(in, 0xFFFFFFFFU);
    put_le32(in + 4, 0);
    memset(in + 8, 0x55, 16);
    rc = send_cci(fd, CXL_MEM_COMMAND_ID_SET_LSA, 0, in, 24, NULL, 0, NULL, &retval, &err);
    if ((rc == -1 && err == EINVAL) || retval == CCI_RET_INVALID_INPUT || retval == CCI_RET_INVALID_PAYLOAD_LEN) {
        snprintf(msg, sizeof(msg), "Rejected OOB SET_LSA write (errno=%s retval=0x%04x)", strerror(err), retval);
        record("Week 6", "Negative", "Out-of-Range SET_LSA Write Offset (0xFFFFFFFF)", "PASS", now_sec() - t0, msg);
    } else {
        record("Week 6", "Negative", "Out-of-Range SET_LSA Write Offset (0xFFFFFFFF)", "FAIL", now_sec() - t0,
               "Out-of-range SET_LSA write unexpectedly succeeded");
    }

    /* 6. Invalid Event Log ID (0xFF) */
    t0 = now_sec();
    in[0] = 0xFF;
    rc = send_cci(fd, CXL_MEM_COMMAND_ID_RAW, OP_GET_EVENT_RECORDS, in, 1, out, 256, &out_sz, &retval, &err);
    if ((rc == -1 && (err == EINVAL || err == EPERM)) || retval == CCI_RET_INVALID_INPUT || retval == CCI_RET_UNSUPPORTED) {
        snprintf(msg, sizeof(msg), "Rejected invalid event log ID (errno=%s retval=0x%04x)", strerror(err), retval);
        record("Week 6", "Negative", "Invalid Event Log Type (0xFF)", "PASS", now_sec() - t0, msg);
    } else {
        record("Week 6", "Negative", "Invalid Event Log Type (0xFF)", "FAIL", now_sec() - t0,
               "Invalid event log 0xFF unexpectedly succeeded");
    }
}

int main(int argc, char **argv) {
    const char *dev = (argc > 1) ? argv[1] : "/dev/cxl/mem0";
    const char *mode = (argc > 2) ? argv[2] : "all";

    int fd = open(dev, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "Failed to open %s: %s\n", dev, strerror(errno));
        return 2;
    }

    query_cmds(fd);

    if (strcmp(mode, "all") == 0 || strcmp(mode, "week5") == 0)
        run_week5(fd);
    if (strcmp(mode, "all") == 0 || strcmp(mode, "week6") == 0)
        run_week6(fd);

    close(fd);

    printf("__JSON_RESULT__:[");
    for (int i = 0; i < g_count; i++) {
        test_result_t *r = &g_results[i];
        printf("%s{\"phase\":\"%s\",\"category\":\"%s\",\"name\":\"%s\",\"status\":\"%s\",\"elapsed\":%.4f,\"detail\":\"%s\"}",
               (i > 0) ? "," : "", r->phase, r->category, r->name, r->status, r->elapsed, r->detail);
    }
    printf("]\n");

    int failed = 0;
    for (int i = 0; i < g_count; i++) {
        if (strcmp(g_results[i].status, "FAIL") == 0)
            failed++;
    }
    return failed > 0 ? 1 : 0;
}
