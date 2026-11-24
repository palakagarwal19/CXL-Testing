#include "cxl_test_common.h"

int main(int argc, char **argv)
{
    cxl_test_ctx ctx;
    const char *device;
    uint8_t out[64] = {0};
    uint32_t out_size = 0;
    uint16_t temperature;
    uint32_t dirty_shutdowns, corrected_volatile, corrected_persistent;
    int first_option, available, rc;
    char detail[256];

    device = cxl_device_arg(argc, argv, &first_option);
    if (first_option != argc) {
        fprintf(stderr, "Usage: %s [device]\n", argv[0]);
        return 2;
    }
    if (cxl_test_begin(&ctx, "GET_HEALTH_INFO", "health_info.log",
                       "health_info_result.txt", device) < 0)
        return cxl_test_end(&ctx, "FAIL", "Could not open CXL device or create log");

    available = cxl_require_command(&ctx, CXL_MEM_COMMAND_ID_GET_HEALTH_INFO);
    if (available == 0)
        return cxl_test_end(&ctx, "SKIP", "GET_HEALTH_INFO is not enabled");

    rc = cxl_send(&ctx, CXL_MEM_COMMAND_ID_GET_HEALTH_INFO, 0, NULL, 0,
                  out, sizeof(out), &out_size);
    if (rc != 0)
        return cxl_test_end(&ctx, "FAIL", "GET_HEALTH_INFO ioctl or mailbox command failed");
    if (out_size != 18)
        return cxl_test_end(&ctx, "FAIL", "Health response length was not 18 bytes");

    temperature = cxl_get_le16(out + 4);
    dirty_shutdowns = cxl_get_le32(out + 6);
    corrected_volatile = cxl_get_le32(out + 10);
    corrected_persistent = cxl_get_le32(out + 14);
    cxl_log(&ctx, "Health status: 0x%02x\nMedia status: 0x%02x\nAdditional status: 0x%02x\n",
            out[0], out[1], out[2]);
    cxl_log(&ctx, "Life used: %u%% (0xff means unknown)\nTemperature: %u (raw CXL units)\n",
            out[3], temperature);
    cxl_log(&ctx, "Dirty shutdowns: %u\nCorrected volatile errors: %u\nCorrected persistent errors: %u\n",
            dirty_shutdowns, corrected_volatile, corrected_persistent);
    if (out[3] > 100 && out[3] != 0xff)
        return cxl_test_end(&ctx, "FAIL", "Life-used field is outside the valid range (0..100 or 0xff)");

    snprintf(detail, sizeof(detail),
             "18-byte health payload valid; health=0x%02x media=0x%02x life_used=%u",
             out[0], out[1], out[3]);
    return cxl_test_end(&ctx, "PASS", detail);
}
