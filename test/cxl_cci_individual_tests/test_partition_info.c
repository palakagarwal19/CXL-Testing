#include "cxl_test_common.h"

int main(int argc, char **argv)
{
    cxl_test_ctx ctx;
    const char *device;
    uint8_t out[64] = {0};
    uint32_t out_size = 0;
    uint64_t active_v, active_p, next_v, next_p;
    int first_option, available, rc;
    char detail[256];

    device = cxl_device_arg(argc, argv, &first_option);
    if (first_option != argc) {
        fprintf(stderr, "Usage: %s [device]\n", argv[0]);
        return 2;
    }
    if (cxl_test_begin(&ctx, "GET_PARTITION_INFO", "partition_info.log",
                       "partition_info_result.txt", device) < 0)
        return cxl_test_end(&ctx, "FAIL", "Could not open CXL device or create log");

    available = cxl_require_command(&ctx, CXL_MEM_COMMAND_ID_GET_PARTITION_INFO);
    if (available == 0)
        return cxl_test_end(&ctx, "SKIP", "GET_PARTITION_INFO is not enabled");

    rc = cxl_send(&ctx, CXL_MEM_COMMAND_ID_GET_PARTITION_INFO, 0, NULL, 0,
                  out, sizeof(out), &out_size);
    if (rc != 0)
        return cxl_test_end(&ctx, "FAIL", "GET_PARTITION_INFO ioctl or mailbox command failed");
    if (out_size != 32)
        return cxl_test_end(&ctx, "FAIL", "Partition response length was not 32 bytes");

    active_v = cxl_get_le64(out);
    active_p = cxl_get_le64(out + 8);
    next_v = cxl_get_le64(out + 16);
    next_p = cxl_get_le64(out + 24);
    cxl_log(&ctx, "Active volatile:   %" PRIu64 " capacity units\n", active_v);
    cxl_log(&ctx, "Active persistent: %" PRIu64 " capacity units\n", active_p);
    cxl_log(&ctx, "Next volatile:     %" PRIu64 " capacity units\n", next_v);
    cxl_log(&ctx, "Next persistent:   %" PRIu64 " capacity units\n", next_p);
    if (active_v > UINT64_MAX - active_p || next_v > UINT64_MAX - next_p)
        return cxl_test_end(&ctx, "FAIL", "Partition capacity sum overflowed");

    snprintf(detail, sizeof(detail),
             "active volatile=%" PRIu64 ", active persistent=%" PRIu64
             ", next volatile=%" PRIu64 ", next persistent=%" PRIu64
             " (units of 256MiB)", active_v, active_p, next_v, next_p);
    return cxl_test_end(&ctx, "PASS", detail);
}
