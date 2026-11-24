# Individual CXL CCI command tests

This directory contains one C program per command/test. The programs use the Linux CXL memory-device ioctl interface from `<linux/cxl_mem.h>`.

## Files

| Program | CCI operation | What it checks |
|---|---|---|
| `test_identify.c` | IDENTIFY (`0x4000`) | 67-byte response; firmware revision, capacity fields and LSA size |
| `test_partition_info.c` | GET_PARTITION_INFO (`0x4100`) | 32-byte response; active and next volatile/persistent capacity fields |
| `test_lsa_get.c` | GET_LSA (`0x4102`) | Requested LSA range can be read and response length matches |
| `test_lsa_set.c` | SET_LSA (`0x4103`) | Saves original bytes, writes a temporary pattern, reads it back, restores original bytes, and verifies restoration |
| `test_health_info.c` | GET_HEALTH_INFO (`0x4200`) | 18-byte response and valid life-used field |
| `test_event_records.c` | RAW + GET_EVENT_RECORDS (`0x0100`) | Queries INFO, WARNING, FAILURE and FATAL logs; validates the 32-byte header and record array length |

`cxl_test_common.h` contains shared ioctl, command-query and logging helpers. Every program produces its own `.log` and `_result.txt` files.

## Build on the RISC-V guest

Copy the directory to your Buildroot/Linux guest, then run:

```sh
make
```

This creates six separate executables. Build on the RISC-V guest itself so the compiler produces a binary for the guest architecture.

## Run the individual tests

Use `sudo` because access to `/dev/cxl/mem0` normally requires root:

```sh
sudo ./test_identify /dev/cxl/mem0
sudo ./test_partition_info /dev/cxl/mem0
sudo ./test_lsa_get /dev/cxl/mem0
sudo ./test_health_info /dev/cxl/mem0
sudo ./test_event_records /dev/cxl/mem0
```

For a non-default LSA range:

```sh
sudo ./test_lsa_get /dev/cxl/mem0 --offset 0 --length 16
```

### SET_LSA test (writes label data temporarily)

Only run this on your disposable QEMU test device. The program backs up the selected LSA bytes, writes a temporary pattern, verifies it, restores the original bytes and verifies the restoration. A power loss or QEMU crash during the write phase could interrupt restoration.

```sh
sudo ./test_lsa_set /dev/cxl/mem0 --write-test --offset 0 --length 16
```

If the kernel marks SET_LSA as exclusive because it owns/manages the LSA, the test reports `SKIP` rather than trying to override the kernel.

## Results and exit codes

Each test writes two files in the current working directory, for example:

- `identify.log`
- `identify_result.txt`

Exit codes:

- `0`: PASS
- `1`: FAIL
- `77`: SKIP (command unsupported, disabled or exclusive)

The `GET_EVENT_RECORDS` test sends the RAW command with selector values 0 through 3. It does not clear or acknowledge event records.

## Notes

- Run the programs from inside the QEMU guest that owns the CXL device.
- The tests do not change memory partitioning.
- GET_LSA defaults to offset 0 and length 16 bytes.
- SET_LSA is deliberately opt-in because it modifies label storage, even though it attempts to restore the original bytes.
- A successful local CCI test validates the local emulated CXL device; it does not by itself verify remote CXL access over Warp-Pipe.
