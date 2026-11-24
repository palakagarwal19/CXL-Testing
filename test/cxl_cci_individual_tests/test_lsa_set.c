#include "cxl_test_common.h"

static int lsa_get(cxl_test_ctx *ctx, uint32_t offset, uint32_t length, uint8_t *data)
{
    uint8_t req[8];
    uint32_t actual = 0;
    cxl_put_le32(req, offset);
    cxl_put_le32(req + 4, length);
    if (cxl_send(ctx, CXL_MEM_COMMAND_ID_GET_LSA, 0, req, sizeof(req),
                 data, length, &actual) != 0)
        return -1;
    return actual == length ? 0 : -1;
}

static int lsa_set(cxl_test_ctx *ctx, uint32_t offset, const uint8_t *data, uint32_t length)
{
    uint8_t *req = calloc(1, 8U + length);
    uint32_t actual = 0;
    int rc;
    if (!req)
        return -1;
    cxl_put_le32(req, offset);
    cxl_put_le32(req + 4, 0); /* reserved */
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
    uint32_t offset = 0, length = 16;
    uint8_t original[CXL_MAX_TEST_PAYLOAD], pattern[CXL_MAX_TEST_PAYLOAD];
    uint8_t verify[CXL_MAX_TEST_PAYLOAD], restored[CXL_MAX_TEST_PAYLOAD];
    bool write_test = false, write_verified = false, restore_verified = false;
    int first_option, i, get_ok, set_ok, available;
    char detail[256];

    device = cxl_device_arg(argc, argv, &first_option);
    for (i = first_option; i < argc; i++) {
        if (strcmp(argv[i], "--write-test") == 0) {
            write_test = true;
        } else if (strcmp(argv[i], "--offset") == 0 && i + 1 < argc) {
            if (cxl_parse_u32(argv[++i], &offset) < 0) { fprintf(stderr, "Invalid offset\n"); return 2; }
        } else if (strcmp(argv[i], "--length") == 0 && i + 1 < argc) {
            if (cxl_parse_u32(argv[++i], &length) < 0) { fprintf(stderr, "Invalid length\n"); return 2; }
        } else {
            fprintf(stderr, "Usage: %s [device] --write-test [--offset N] [--length N]\n", argv[0]);
            return 2;
        }
    }
    if (!write_test) {
        fprintf(stderr, "SET_LSA modifies label data. Run with --write-test only on a test device.\n");
        return 77;
    }
    if (length == 0 || length > CXL_MAX_TEST_PAYLOAD || (uint64_t)offset + length > UINT32_MAX) {
        fprintf(stderr, "Length must be 1..%u and offset+length must fit in 32 bits\n", CXL_MAX_TEST_PAYLOAD);
        return 2;
    }
    if (cxl_test_begin(&ctx, "SET_LSA", "lsa_set.log", "lsa_set_result.txt", device) < 0)
        return cxl_test_end(&ctx, "FAIL", "Could not open CXL device or create log");

    available = cxl_require_command(&ctx, CXL_MEM_COMMAND_ID_SET_LSA);
    if (available == 0)
        return cxl_test_end(&ctx, "SKIP", "SET_LSA is unavailable or kernel marks it exclusive");
    available = cxl_require_command(&ctx, CXL_MEM_COMMAND_ID_GET_LSA);
    if (available == 0)
        return cxl_test_end(&ctx, "SKIP", "GET_LSA is required to back up and verify the label data");

    /* Back up the original bytes before writing anything. */
    if (lsa_get(&ctx, offset, length, original) < 0)
        return cxl_test_end(&ctx, "FAIL", "Could not save original LSA bytes; write was not attempted");

    for (i = 0; (uint32_t)i < length; i++)
        pattern[i] = (uint8_t)(original[i] ^ (uint8_t)(0xA5U + ((uint32_t)i & 0x0fU)));

    set_ok = lsa_set(&ctx, offset, pattern, length);
    if (set_ok == 0 && lsa_get(&ctx, offset, length, verify) == 0 &&
        memcmp(pattern, verify, length) == 0)
        write_verified = true;

    /* Always attempt to restore after the write attempt. */
    if (set_ok == 0 || !write_verified)
        (void)lsa_set(&ctx, offset, original, length);
    get_ok = lsa_get(&ctx, offset, length, restored);
    if (get_ok == 0 && memcmp(original, restored, length) == 0)
        restore_verified = true;

    if (!restore_verified)
        return cxl_test_end(&ctx, "FAIL", "CRITICAL: original LSA bytes were not verified after restoration");
    if (!write_verified)
        return cxl_test_end(&ctx, "FAIL", "SET_LSA write/read-back did not match; original bytes were restored");

    snprintf(detail, sizeof(detail),
             "temporarily wrote and verified %u bytes at offset %u; original bytes restored and verified",
             length, offset);
    return cxl_test_end(&ctx, "PASS", detail);
}
