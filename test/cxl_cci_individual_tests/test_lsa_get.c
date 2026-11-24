#include "cxl_test_common.h"

int main(int argc, char **argv)
{
    cxl_test_ctx ctx;
    const char *device;
    uint32_t offset = 0, length = 16, out_size = 0;
    uint8_t request[8], out[CXL_MAX_TEST_PAYLOAD] = {0};
    int first_option, i, available, rc;
    char detail[200];

    device = cxl_device_arg(argc, argv, &first_option);
    for (i = first_option; i < argc; i++) {
        if (strcmp(argv[i], "--offset") == 0 && i + 1 < argc) {
            if (cxl_parse_u32(argv[++i], &offset) < 0) { fprintf(stderr, "Invalid offset\n"); return 2; }
        } else if (strcmp(argv[i], "--length") == 0 && i + 1 < argc) {
            if (cxl_parse_u32(argv[++i], &length) < 0) { fprintf(stderr, "Invalid length\n"); return 2; }
        } else {
            fprintf(stderr, "Usage: %s [device] [--offset N] [--length N]\n", argv[0]);
            return 2;
        }
    }
    if (length == 0 || length > CXL_MAX_TEST_PAYLOAD || (uint64_t)offset + length > UINT32_MAX) {
        fprintf(stderr, "Length must be 1..%u and offset+length must fit in 32 bits\n", CXL_MAX_TEST_PAYLOAD);
        return 2;
    }
    if (cxl_test_begin(&ctx, "GET_LSA", "lsa_get.log", "lsa_get_result.txt", device) < 0)
        return cxl_test_end(&ctx, "FAIL", "Could not open CXL device or create log");

    available = cxl_require_command(&ctx, CXL_MEM_COMMAND_ID_GET_LSA);
    if (available == 0)
        return cxl_test_end(&ctx, "SKIP", "GET_LSA is not enabled");

    cxl_put_le32(request, offset);
    cxl_put_le32(request + 4, length);
    rc = cxl_send(&ctx, CXL_MEM_COMMAND_ID_GET_LSA, 0, request, sizeof(request),
                  out, length, &out_size);
    if (rc != 0)
        return cxl_test_end(&ctx, "FAIL", "GET_LSA ioctl or mailbox command failed");
    if (out_size != length)
        return cxl_test_end(&ctx, "FAIL", "GET_LSA returned a different length than requested");

    snprintf(detail, sizeof(detail), "read and validated %u bytes from LSA offset %u", out_size, offset);
    return cxl_test_end(&ctx, "PASS", detail);
}
