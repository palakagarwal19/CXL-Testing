#include "cxl_test_common.h"

#define CXL_IDENTIFY_RESPONSE_SIZE    69U
#define CXL_PARTITION_INFO_SIZE       32U
#define CXL_SET_PARTITION_INPUT_SIZE  10U
#define CXL_CAPACITY_UNIT_BYTES       (256ULL * 1024ULL * 1024ULL)

static void put_le64(uint8_t *p, uint64_t value)
{
    cxl_put_le32(p, (uint32_t)value);
    cxl_put_le32(p + 4, (uint32_t)(value >> 32));
}

static int get_identify(cxl_test_ctx *ctx, uint8_t *out, size_t out_capacity)
{
    uint32_t actual = 0;

    if (out_capacity < CXL_IDENTIFY_RESPONSE_SIZE)
        return -1;

    if (cxl_send(ctx, CXL_MEM_COMMAND_ID_IDENTIFY, 0, NULL, 0,
                 out, CXL_IDENTIFY_RESPONSE_SIZE, &actual) != 0)
        return -1;

    return actual == CXL_IDENTIFY_RESPONSE_SIZE ? 0 : -1;
}

static int get_partition_info(cxl_test_ctx *ctx,
                              uint64_t *active_v, uint64_t *active_p,
                              uint64_t *next_v, uint64_t *next_p)
{
    uint8_t out[CXL_PARTITION_INFO_SIZE] = {0};
    uint32_t actual = 0;

    if (cxl_send(ctx, CXL_MEM_COMMAND_ID_GET_PARTITION_INFO, 0,
                 NULL, 0, out, sizeof(out), &actual) != 0)
        return -1;

    if (actual != sizeof(out)) {
        cxl_log(ctx, "[ERROR] GET_PARTITION_INFO returned %u bytes; expected %u.\n",
                actual, (unsigned)sizeof(out));
        return -1;
    }

    /*
     * Output format (each field is 64-bit LE, in multiples of 256 MiB):
     * - Bytes  0..7:  Active Volatile Capacity
     * - Bytes  8..15: Active Persistent Capacity
     * - Bytes 16..23: Next Volatile Capacity (pending reset)
     * - Bytes 24..31: Next Persistent Capacity (pending reset)
     */
    *active_v = cxl_get_le64(out);
    *active_p = cxl_get_le64(out + 8);
    *next_v   = cxl_get_le64(out + 16);
    *next_p   = cxl_get_le64(out + 24);
    return 0;
}

/*
 * Issues CXL_MEM_COMMAND_ID_SET_PARTITION_INFO.
 *
 * Payload format (10 bytes):
 * - Bytes 0..7: Volatile capacity in 256 MiB units (LE64)
 * - Byte 8:     Flags (Bit 0 = Immediate; Bit 0 is 0 for staging on reset)
 * - Byte 9:     Reserved (Must be zero)
 */
static int set_partition_info(cxl_test_ctx *ctx, uint64_t volatile_units)
{
    uint8_t req[CXL_SET_PARTITION_INPUT_SIZE] = {0};
    uint32_t actual = 0;

    put_le64(req, volatile_units);
    req[8] = 0; /* Staged: takes effect upon device warm reset / host reboot */
    req[9] = 0; /* Padding/reserved required by Linux kernel ABI */

    return cxl_send(ctx, CXL_MEM_COMMAND_ID_SET_PARTITION_INFO, 0,
                    req, sizeof(req), NULL, 0, &actual);
}

static int parse_u64(const char *s, uint64_t *value)
{
    char *end = NULL;
    unsigned long long parsed;

    errno = 0;
    parsed = strtoull(s, &end, 0);
    if (errno || !end || *end != '\0')
        return -1;
    *value = (uint64_t)parsed;
    return 0;
}

int main(int argc, char **argv)
{
    cxl_test_ctx ctx;
    const char *device = NULL;
    uint8_t identify[CXL_IDENTIFY_RESPONSE_SIZE] = {0};
    uint64_t total_units, volatile_only, persistent_only, partition_alignment;
    uint64_t active_v, active_p, next_v, next_p;
    uint64_t requested_v = UINT64_MAX;
    uint64_t requested_p;
    uint64_t verify_v, verify_p, verify_next_v, verify_next_p;
    uint64_t restore_v, restore_p, restore_next_v, restore_next_p;
    bool keep_pending = false;
    int i, available;
    char detail[320];

    /* Parse arguments directly */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--volatile-units") == 0 && i + 1 < argc) {
            if (parse_u64(argv[++i], &requested_v) < 0) {
                fprintf(stderr, "Invalid --volatile-units value\n");
                return 2;
            }
        } else if (strcmp(argv[i], "--keep-pending") == 0) {
            keep_pending = true;
        } else if (argv[i][0] != '-') {
            device = argv[i];
        } else {
            fprintf(stderr, "Usage: %s <device> --volatile-units N [--keep-pending]\n", argv[0]);
            return 2;
        }
    }

    if (!device) {
        fprintf(stderr, "Error: No CXL device specified.\nUsage: %s <device> --volatile-units N [--keep-pending]\n", argv[0]);
        return 2;
    }

    if (requested_v == UINT64_MAX) {
        fprintf(stderr, "Error: Specify --volatile-units N.\n");
        return 2;
    }

    /* Initialize test harness with guaranteed valid device pointer */
    if (cxl_test_begin(&ctx, "SET_PARTITION_INFO", "set_partition.log",
                       "set_partition_result.txt", device) < 0) {
        fprintf(stderr, "Failed to open device %s\n", device);
        return 1;
    }
    /* Verify kernel driver command availability */
    available = cxl_require_command(&ctx, CXL_MEM_COMMAND_ID_SET_PARTITION_INFO);
    if (available == 0)
        return cxl_test_end(&ctx, "SKIP", "SET_PARTITION_INFO is not supported by device/driver");

    available = cxl_require_command(&ctx, CXL_MEM_COMMAND_ID_IDENTIFY);
    if (available == 0)
        return cxl_test_end(&ctx, "SKIP", "IDENTIFY is required to validate partition capabilities");

    available = cxl_require_command(&ctx, CXL_MEM_COMMAND_ID_GET_PARTITION_INFO);
    if (available == 0)
        return cxl_test_end(&ctx, "SKIP", "GET_PARTITION_INFO is required to verify partition layout");

    if (get_identify(&ctx, identify, sizeof(identify)) < 0)
        return cxl_test_end(&ctx, "FAIL", "Could not retrieve IDENTIFY response");

    /*
     * CXL Identify Payload field offsets:
     * - Bytes  0..15: FW Revision
     * - Bytes 16..23: Total Capacity (LE64, 256 MiB units)
     * - Bytes 24..31: Volatile Only Capacity (LE64)
     * - Bytes 32..39: Persistent Only Capacity (LE64)
     * - Bytes 40..47: Partition Alignment (LE64, 256 MiB units)
     */
    total_units         = cxl_get_le64(identify + 16);
    volatile_only       = cxl_get_le64(identify + 24);
    persistent_only     = cxl_get_le64(identify + 32);
    partition_alignment = cxl_get_le64(identify + 40);

    cxl_log(&ctx, "[INFO] Total capacity: %" PRIu64 " units (%" PRIu64 " MiB)\n",
            total_units, total_units * 256ULL);
    cxl_log(&ctx, "[INFO] Volatile capacity: %" PRIu64 " units, Persistent capacity: %" PRIu64 " units\n",
            volatile_only, persistent_only);
    cxl_log(&ctx, "[INFO] Partition alignment: %" PRIu64 " capacity units\n",
            partition_alignment);

    if (total_units == 0)
        return cxl_test_end(&ctx, "SKIP", "Device reports zero total capacity");

    /* If alignment is 0, dynamic partition changes are not allowed by hardware */
    if (partition_alignment == 0)
        return cxl_test_end(&ctx, "SKIP",
                            "partition_alignment=0; device has fixed partitioning and does not support reconfiguration");

    if (requested_v > total_units) {
        snprintf(detail, sizeof(detail),
                 "Requested volatile units (%" PRIu64 ") exceeds total units (%" PRIu64 ")",
                 requested_v, total_units);
        return cxl_test_end(&ctx, "FAIL", detail);
    }

    if (requested_v % partition_alignment != 0) {
        snprintf(detail, sizeof(detail),
                 "Requested volatile units (%" PRIu64 ") not aligned to partition_alignment (%" PRIu64 ")",
                 requested_v, partition_alignment);
        return cxl_test_end(&ctx, "FAIL", detail);
    }

    if (get_partition_info(&ctx, &active_v, &active_p, &next_v, &next_p) < 0)
        return cxl_test_end(&ctx, "FAIL", "Could not read current partition layout");

    cxl_log(&ctx, "[INFO] Active layout: Volatile=%" PRIu64 ", Persistent=%" PRIu64 "\n",
            active_v, active_p);
    cxl_log(&ctx, "[INFO] Pending layout: Next Volatile=%" PRIu64 ", Next Persistent=%" PRIu64 "\n",
            next_v, next_p);

    if (active_v + active_p != total_units)
        return cxl_test_end(&ctx, "FAIL", "Active capacities do not equal total capacity reported by IDENTIFY");

    if (next_v != 0 || next_p != 0)
        return cxl_test_end(&ctx, "SKIP", "A partition reconfiguration is already pending reset");

    requested_p = total_units - requested_v;
    cxl_log(&ctx, "[INFO] Staging next partition: Volatile=%" PRIu64 ", Persistent=%" PRIu64 "\n",
            requested_v, requested_p);

    /* Stage partition change */
    if (set_partition_info(&ctx, requested_v) != 0)
        return cxl_test_end(&ctx, "FAIL", "SET_PARTITION_INFO mailbox command was rejected by device");

    /* Verify staging using GET_PARTITION_INFO */
    if (get_partition_info(&ctx, &verify_v, &verify_p, &verify_next_v, &verify_next_p) < 0) {
        if (!keep_pending)
            (void)set_partition_info(&ctx, active_v);
        return cxl_test_end(&ctx, "FAIL", "SET_PARTITION_INFO succeeded, but read-back failed");
    }

    if (verify_next_v != requested_v || verify_next_p != requested_p) {
        if (!keep_pending)
            (void)set_partition_info(&ctx, active_v);
        return cxl_test_end(&ctx, "FAIL", "Staged pending values did not match requested values");
    }

    cxl_log(&ctx, "[PASS] SET_PARTITION_INFO staged successfully.\n");

    if (keep_pending) {
        snprintf(detail, sizeof(detail),
                 "Staged pending configuration: Volatile=%" PRIu64 " units, Persistent=%" PRIu64 " units (requires device reset)",
                 requested_v, requested_p);
        return cxl_test_end(&ctx, "PASS", detail);
    }

    /* Test teardown: restore original active setting */
    cxl_log(&ctx, "[INFO] Restoring pending configuration back to original active allocation...\n");
    if (set_partition_info(&ctx, active_v) != 0)
        return cxl_test_end(&ctx, "FAIL", "Failed to restore original partition configuration");

    if (get_partition_info(&ctx, &restore_v, &restore_p, &restore_next_v, &restore_next_p) < 0)
        return cxl_test_end(&ctx, "FAIL", "Could not verify restored partition state");

    if (!((restore_next_v == active_v && restore_next_p == active_p) ||
          (restore_next_v == 0 && restore_next_p == 0))) {
        return cxl_test_end(&ctx, "FAIL", "Restoration read-back mismatch");
    }

    snprintf(detail, sizeof(detail),
             "Verified staging of Volatile=%" PRIu64 " units; successfully restored initial state",
             requested_v);
    return cxl_test_end(&ctx, "PASS", detail);
}
