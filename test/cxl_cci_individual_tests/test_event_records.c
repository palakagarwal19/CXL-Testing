#include "cxl_test_common.h"

#define CXL_GET_EVENT_RECORDS_OPCODE 0x0100U
#define CXL_EVENT_HEADER_SIZE 32U
#define CXL_EVENT_RECORD_SIZE 128U

int main(int argc, char **argv)
{
    static const char *log_names[] = { "INFO", "WARNING", "FAILURE", "FATAL" };
    cxl_test_ctx ctx;
    const char *device;
    uint8_t input[1], out[CXL_MAX_TEST_PAYLOAD];
    uint32_t total_records = 0;
    bool failed = false;
    int first_option, available;
    unsigned int log_index;

    device = cxl_device_arg(argc, argv, &first_option);
    if (first_option != argc) {
        fprintf(stderr, "Usage: %s [device]\n", argv[0]);
        return 2;
    }
    if (cxl_test_begin(&ctx, "GET_EVENT_RECORDS", "event_records.log",
                       "event_records_result.txt", device) < 0)
        return cxl_test_end(&ctx, "FAIL", "Could not open CXL device or create log");

    available = cxl_require_command(&ctx, CXL_MEM_COMMAND_ID_RAW);
    if (available == 0)
        return cxl_test_end(&ctx, "SKIP", "RAW command interface is not enabled; cannot send opcode 0x0100");

    for (log_index = 0; log_index < 4; log_index++) {
        uint32_t out_size = 0;
        uint16_t record_count;
        size_t required;
        int rc;

        memset(out, 0, sizeof(out));
        input[0] = (uint8_t)log_index;
        cxl_log(&ctx, "\n[INFO] Requesting %s event log (selector=%u)\n",
                log_names[log_index], log_index);
        rc = cxl_send(&ctx, CXL_MEM_COMMAND_ID_RAW, CXL_GET_EVENT_RECORDS_OPCODE,
                      input, sizeof(input), out, sizeof(out), &out_size);
        if (rc != 0) {
            cxl_log(&ctx, "[FAIL] %s event-log request failed\n", log_names[log_index]);
            failed = true;
            continue;
        }
        if (out_size < CXL_EVENT_HEADER_SIZE) {
            cxl_log(&ctx, "[FAIL] %s response shorter than %u-byte header\n",
                    log_names[log_index], CXL_EVENT_HEADER_SIZE);
            failed = true;
            continue;
        }
        record_count = cxl_get_le16(out + 20);
        required = CXL_EVENT_HEADER_SIZE + (size_t)record_count * CXL_EVENT_RECORD_SIZE;
        if (required > out_size) {
            cxl_log(&ctx, "[FAIL] %s: header says %u records, requiring %zu bytes; received %u\n",
                    log_names[log_index], record_count, required, out_size);
            failed = true;
            continue;
        }
        total_records += record_count;
        cxl_log(&ctx, "[PASS] %s: %u record(s), flags=0x%02x, response=%u bytes\n",
                log_names[log_index], record_count, out[0], out_size);
    }

    if (failed)
        return cxl_test_end(&ctx, "FAIL", "One or more event log responses failed validation");
    {
        char detail[200];
        snprintf(detail, sizeof(detail),
                 "INFO, WARNING, FAILURE and FATAL logs validated; %u total record(s) returned",
                 total_records);
        return cxl_test_end(&ctx, "PASS", detail);
    }
}
