#include "cxl_test_common.h"

/*
 * Issues CXL_MEM_COMMAND_ID_SET_LSA to write bytes into the LSA permanently.
 */
static int lsa_set_permanent(cxl_test_ctx *ctx, uint32_t offset, const uint8_t *data, uint32_t length)
{
    uint8_t *req;
    uint32_t actual = 0;
    int rc;

    if (!ctx || !data || length == 0)
        return -1;

    /* 8-byte header: 4 bytes offset + 4 bytes reserved (zeroed) + data bytes */
    req = calloc(1, 8U + length);
    if (!req)
        return -1;

    cxl_put_le32(req, offset);
    cxl_put_le32(req + 4, 0); /* Reserved */
    memcpy(req + 8, data, length);

    rc = cxl_send(ctx, CXL_MEM_COMMAND_ID_SET_LSA, 0, req, 8U + length,
                  NULL, 0, &actual);

    free(req);
    return rc;
}

int main(int argc, char **argv)
{
    cxl_test_ctx ctx;
    const char *device;
    uint32_t offset = 0;
    uint32_t length = 0;
    uint8_t buffer[CXL_MAX_TEST_PAYLOAD];
    int first_option, i;

    device = cxl_device_arg(argc, argv, &first_option);

    /* Default test payload if user doesn't pass one */
    const char *default_msg = "TEST_CXL_LABEL!";
    length = (uint32_t)strlen(default_msg);
    memcpy(buffer, default_msg, length);

    /* Parse CLI options: [--offset N] [--data STRING] */
    for (i = first_option; i < argc; i++) {
        if (strcmp(argv[i], "--offset") == 0 && i + 1 < argc) {
            if (cxl_parse_u32(argv[++i], &offset) < 0) {
                fprintf(stderr, "Invalid offset\n");
                return 2;
            }
        } else if (strcmp(argv[i], "--data") == 0 && i + 1 < argc) {
            const char *user_data = argv[++i];
            length = (uint32_t)strlen(user_data);
            if (length > CXL_MAX_TEST_PAYLOAD) {
                fprintf(stderr, "Data exceeds max payload (%u)\n", CXL_MAX_TEST_PAYLOAD);
                return 2;
            }
            memcpy(buffer, user_data, length);
        } else {
            fprintf(stderr, "Usage: %s <device> [--offset N] [--data STRING]\n", argv[0]);
            return 2;
        }
    }

    if (cxl_test_begin(&ctx, "PERMANENT_SET_LSA", "lsa_write.log", "lsa_write_result.txt", device) < 0) {
        fprintf(stderr, "Could not open CXL device %s\n", device);
        return 1;
    }

    if (!cxl_require_command(&ctx, CXL_MEM_COMMAND_ID_SET_LSA)) {
        fprintf(stderr, "SET_LSA is unavailable on this device\n");
        cxl_test_end(&ctx, "FAIL", "SET_LSA unavailable");
        return 1;
    }

    printf("Writing %u bytes to offset %u of %s...\n", length, offset, device);
    if (lsa_set_permanent(&ctx, offset, buffer, length) != 0) {
        fprintf(stderr, "SET_LSA command failed!\n");
        cxl_test_end(&ctx, "FAIL", "SET_LSA ioctl error");
        return 1;
    }

    printf("Successfully wrote data to LSA (permanent, no restore).\n");
    return cxl_test_end(&ctx, "PASS", "Data written successfully");
}
