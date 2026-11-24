#include "cxl_test_common.h"

int main(int argc, char **argv)
{
    cxl_test_ctx ctx;
    const char *device;
    uint8_t out[128] = {0};
    uint32_t out_size = 0;
    int first_option, available, rc;
    char detail[256];

    device = cxl_device_arg(argc, argv, &first_option);
    if (first_option != argc) {
        fprintf(stderr, "Usage: %s [device]\n", argv[0]);
        return 2;
    }
    if (cxl_test_begin(&ctx, "IDENTIFY", "identify.log", "identify_result.txt", device) < 0)
        return cxl_test_end(&ctx, "FAIL", "Could not open CXL device or create log");

    available = cxl_require_command(&ctx, CXL_MEM_COMMAND_ID_IDENTIFY);
    if (available == 0)
        return cxl_test_end(&ctx, "SKIP", "IDENTIFY is not enabled");

    rc = cxl_send(&ctx, CXL_MEM_COMMAND_ID_IDENTIFY, 0, NULL, 0,
                  out, sizeof(out), &out_size);
    if (rc != 0)
        return cxl_test_end(&ctx, "FAIL", "IDENTIFY ioctl or mailbox command failed");
    if (out_size != 69)
        return cxl_test_end(&ctx, "FAIL", "IDENTIFY response length was not 69 bytes");

    snprintf(detail, sizeof(detail),
             "firmware='%.16s'; total_capacity=%" PRIu64 " x 256MiB; volatile=%" PRIu64
             "; persistent=%" PRIu64 "; partition_alignment=%" PRIu64 "; LSA_size=%u bytes",
             (const char *)out, cxl_get_le64(out + 16), cxl_get_le64(out + 24),
             cxl_get_le64(out + 32), cxl_get_le64(out + 40), cxl_get_le32(out + 56));
    cxl_log(&ctx, "[PASS] %s\n", detail);
    return cxl_test_end(&ctx, "PASS", detail);
}
