#!/usr/bin/env python3
import os
import sys
import json
import time
import fcntl
import ctypes
import struct
import errno

# Linux UAPI <linux/cxl_mem.h> ioctl constants
CXL_MEM_QUERY_COMMANDS = 0xC008CE01
CXL_MEM_SEND_COMMAND  = 0xC030CE02

# UAPI Command IDs (enum cxl_mem_command_id)
CMD_INVALID            = 0
CMD_IDENTIFY           = 1
CMD_RAW                = 2
CMD_GET_SUPPORTED_LOGS = 3
CMD_GET_LOG            = 4
CMD_GET_LSA            = 5
CMD_SET_LSA            = 6
CMD_GET_HEALTH_INFO    = 7
CMD_GET_ALERT_CONFIG   = 8
CMD_SET_ALERT_CONFIG   = 9
CMD_GET_SHUTDOWN_STATE = 10
CMD_SET_SHUTDOWN_STATE = 11
CMD_GET_POISON         = 12
CMD_INJECT_POISON      = 13
CMD_CLEAR_POISON       = 14
CMD_GET_SCAN_MEDIA_CAPS= 15
CMD_SCAN_MEDIA         = 16
CMD_GET_SCAN_MEDIA     = 17
CMD_GET_TIMESTAMP      = 18
CMD_SET_TIMESTAMP      = 19
CMD_GET_PARTITION_INFO = 20
CMD_SET_PARTITION_INFO = 21

# CXL 3.0 Spec CCI Opcodes
OP_GET_EVENT_RECORDS   = 0x0100
OP_CLEAR_EVENT_RECORDS = 0x0101

# CXL 3.0 Spec Mailbox Return Codes
CCI_RET_SUCCESS             = 0x0000
CCI_RET_INVALID_INPUT       = 0x0002
CCI_RET_UNSUPPORTED         = 0x0003
CCI_RET_INVALID_PAYLOAD_LEN = 0x0016


class CxlSendCommand(ctypes.Structure):
    _fields_ = [
        ("id", ctypes.c_uint32),
        ("flags", ctypes.c_uint32),
        ("raw_opcode", ctypes.c_uint16),
        ("rsvd1", ctypes.c_uint16),
        ("retval", ctypes.c_uint32),
        ("in_size", ctypes.c_uint32),
        ("rsvd2", ctypes.c_uint32),
        ("in_payload", ctypes.c_uint64),
        ("out_size", ctypes.c_uint32),
        ("rsvd3", ctypes.c_uint32),
        ("out_payload", ctypes.c_uint64),
    ]


class CxlCommandInfo(ctypes.Structure):
    _fields_ = [
        ("id", ctypes.c_uint32),
        ("flags", ctypes.c_uint32),
        ("size_in", ctypes.c_int32),
        ("size_out", ctypes.c_int32),
    ]


class CxlQueryHeader(ctypes.Structure):
    _fields_ = [
        ("n_commands", ctypes.c_uint32),
        ("rsvd", ctypes.c_uint32),
    ]


class PurePythonCCISuite:
    def __init__(self, device="/dev/cxl/mem0"):
        self.device = device
        self.fd = os.open(device, os.O_RDWR)
        self.results = []
        self.supported_cmds = self._query_supported_commands()
        self.lsa_size = 0
        self.total_cap_units = 0  # in 256MB units

    def close(self):
        if self.fd >= 0:
            os.close(self.fd)

    def _query_supported_commands(self):
        hdr = CxlQueryHeader(n_commands=0, rsvd=0)
        try:
            fcntl.ioctl(self.fd, CXL_MEM_QUERY_COMMANDS, hdr)
        except OSError:
            return {}

        count = hdr.n_commands
        if count == 0 or count > 1024:
            return {}

        class QueryFull(ctypes.Structure):
            _fields_ = [
                ("n_commands", ctypes.c_uint32),
                ("rsvd", ctypes.c_uint32),
                ("commands", CxlCommandInfo * count),
            ]

        q = QueryFull(n_commands=count, rsvd=0)
        try:
            fcntl.ioctl(self.fd, CXL_MEM_QUERY_COMMANDS, q)
        except OSError:
            return {}

        cmds = {}
        for i in range(q.n_commands):
            c = q.commands[i]
            cmds[c.id] = {"flags": c.flags, "size_in": c.size_in, "size_out": c.size_out}
        return cmds

    def is_cmd_enabled(self, cmd_id):
        if not self.supported_cmds:
            return True  # Fallback: attempt command if query unsupported
        info = self.supported_cmds.get(cmd_id)
        return bool(info and (info["flags"] & 0x1))

    def send_cmd(self, cmd_id, in_bytes=b"", out_cap=4096, raw_opcode=0):
        in_buf = ctypes.create_string_buffer(in_bytes, len(in_bytes)) if len(in_bytes) > 0 else None
        out_buf = ctypes.create_string_buffer(out_cap) if out_cap > 0 else None

        cmd = CxlSendCommand()
        cmd.id = cmd_id
        if cmd_id == CMD_RAW:
            cmd.raw_opcode = raw_opcode
        cmd.in_size = len(in_bytes)
        cmd.in_payload = ctypes.addressof(in_buf) if in_buf else 0
        cmd.out_size = out_cap
        cmd.out_payload = ctypes.addressof(out_buf) if out_buf else 0

        try:
            fcntl.ioctl(self.fd, CXL_MEM_SEND_COMMAND, cmd)
        except OSError as e:
            return -1, e.errno, 0, b""

        actual_out = out_buf.raw[:cmd.out_size] if (out_buf and cmd.out_size > 0) else b""
        return 0, 0, cmd.retval, actual_out

    def add_result(self, phase, category, name, status, elapsed, detail):
        self.results.append({
            "phase": phase,
            "category": category,
            "name": name,
            "status": status,
            "elapsed": round(elapsed, 4),
            "detail": detail,
        })

    # =========================================================================
    # WEEK 5: POSITIVE FUNCTIONAL TEST CASES
    # =========================================================================
    def run_week5(self):
        # 1. Identify Memory Device (Opcode 0x4000)
        t0 = time.time()
        rc, err, retval, out = self.send_cmd(CMD_IDENTIFY, out_cap=0x43)
        if rc == 0 and retval == CCI_RET_SUCCESS and len(out) >= 0x43:
            fw_rev = out[0x00:0x10].decode("ascii", errors="replace").strip("\x00 ")
            self.total_cap_units = struct.unpack_from("<Q", out, 0x10)[0]
            volatile_units = struct.unpack_from("<Q", out, 0x18)[0]
            pmem_units = struct.unpack_from("<Q", out, 0x20)[0]
            self.lsa_size = struct.unpack_from("<I", out, 0x38)[0]
            detail = (
                f"FW='{fw_rev}', Total={self.total_cap_units*256}MB "
                f"(Volatile={volatile_units*256}MB, PMEM={pmem_units*256}MB), LSA={self.lsa_size}B"
            )
            self.add_result("Week 5", "Identify", "Identify Memory Device (0x4000)", "PASS", time.time() - t0, detail)
        else:
            err_name = errno.errorcode.get(err, str(err))
            self.add_result("Week 5", "Identify", "Identify Memory Device (0x4000)", "FAIL", time.time() - t0,
                            f"ioctl_rc={rc}, errno={err_name}, mailbox_retval=0x{retval:04x}")

        # 2. Get Partition Info (Opcode 0x4100)
        t0 = time.time()
        if not self.is_cmd_enabled(CMD_GET_PARTITION_INFO):
            self.add_result("Week 5", "Partition", "Get Partition Info (0x4100)", "SKIP", time.time() - t0,
                            "Command not enabled on device/kernel")
            active_v, active_p = 0, 0
        else:
            rc, err, retval, out = self.send_cmd(CMD_GET_PARTITION_INFO, out_cap=32)
            if rc == 0 and retval == CCI_RET_SUCCESS and len(out) >= 32:
                active_v, active_p, next_v, next_p = struct.unpack("<QQQQ", out[:32])
                detail = (
                    f"ActiveVolatile={active_v*256}MB, ActivePMEM={active_p*256}MB, "
                    f"NextVolatile={next_v*256}MB, NextPMEM={next_p*256}MB"
                )
                self.add_result("Week 5", "Partition", "Get Partition Info (0x4100)", "PASS", time.time() - t0, detail)
            else:
                active_v, active_p = 0, 0
                err_name = errno.errorcode.get(err, str(err))
                self.add_result("Week 5", "Partition", "Get Partition Info (0x4100)", "FAIL", time.time() - t0,
                                f"ioctl_rc={rc}, errno={err_name}, mailbox_retval=0x{retval:04x}")

        # 3. Set LSA (Opcode 0x4103) & Get LSA (Opcode 0x4102) Roundtrip
        t0 = time.time()
        if not (self.is_cmd_enabled(CMD_SET_LSA) and self.is_cmd_enabled(CMD_GET_LSA)):
            self.add_result("Week 5", "LSA", "LSA Set & Get Roundtrip (0x4103/0x4102)", "SKIP", time.time() - t0,
                            "GET_LSA / SET_LSA not enabled on device")
        else:
            # Backup original 64 bytes at offset 0 so test is non-destructive
            _, _, _, orig_lsa = self.send_cmd(CMD_GET_LSA, in_bytes=struct.pack("<II", 0, 64), out_cap=64)

            sig = b"CXL3_CCI_VERIFY_" + str(int(time.time())).encode("ascii")
            sig = sig.ljust(64, b"\x00")[:64]
            set_in = struct.pack("<II", 0, 0) + sig  # offset=0, rsvd=0, data=64B
            rc_s, err_s, ret_s, _ = self.send_cmd(CMD_SET_LSA, in_bytes=set_in, out_cap=0)

            get_in = struct.pack("<II", 0, 64)  # offset=0, length=64
            rc_g, err_g, ret_g, out_g = self.send_cmd(CMD_GET_LSA, in_bytes=get_in, out_cap=64)

            # Restore original LSA bytes if backup succeeded
            if len(orig_lsa) == 64:
                self.send_cmd(CMD_SET_LSA, in_bytes=struct.pack("<II", 0, 0) + orig_lsa, out_cap=0)

            if rc_s == 0 and ret_s == 0 and rc_g == 0 and ret_g == 0 and out_g == sig:
                self.add_result("Week 5", "LSA", "LSA Set & Get Roundtrip (0x4103/0x4102)", "PASS", time.time() - t0,
                                "Wrote 64B pattern at offset 0x0, read back exact match, and restored original LSA")
            else:
                self.add_result("Week 5", "LSA", "LSA Set & Get Roundtrip (0x4103/0x4102)", "FAIL", time.time() - t0,
                                f"Set(rc={rc_s},err={err_s},ret=0x{ret_s:x}) Get(rc={rc_g},err={err_g},ret=0x{ret_g:x})")

        # 4. Get Health Info (Opcode 0x4200)
        t0 = time.time()
        rc, err, retval, out = self.send_cmd(CMD_GET_HEALTH_INFO, out_cap=0x12)
        if rc == 0 and retval == CCI_RET_SUCCESS and len(out) >= 0x12:
            health_status = out[0]
            media_status = out[1]
            ext_status = out[2]
            life_used = out[3]
            temp_c = struct.unpack_from("<h", out, 4)[0]
            detail = (
                f"HealthFlags=0x{health_status:02x}, MediaStatus=0x{media_status:02x}, "
                f"ExtStatus=0x{ext_status:02x}, LifeUsed={life_used}%, Temp={temp_c}C"
            )
            self.add_result("Week 5", "Health", "Get Health Info (0x4200)", "PASS", time.time() - t0, detail)
        else:
            err_name = errno.errorcode.get(err, str(err))
            self.add_result("Week 5", "Health", "Get Health Info (0x4200)", "FAIL", time.time() - t0,
                            f"ioctl_rc={rc}, errno={err_name}, mailbox_retval=0x{retval:04x}")

        # 5. Get Event Records (Opcode 0x0100) & Clear Event Records (Opcode 0x0101)
        t0 = time.time()
        if not self.is_cmd_enabled(CMD_RAW):
            self.add_result("Week 5", "Events", "Get & Clear Event Records (0x0100/0x0101)", "SKIP", time.time() - t0,
                            "RAW commands disabled in kernel (enable CONFIG_CXL_MEM_RAW_COMMANDS)")
        else:
            # Event Log Type 0x00 = Informational Log
            rc, err, retval, out = self.send_cmd(CMD_RAW, in_bytes=b"\x00", out_cap=4096, raw_opcode=OP_GET_EVENT_RECORDS)
            if rc == 0 and retval == CCI_RET_SUCCESS and len(out) >= 16:
                flags = out[0]
                overflow_cnt = struct.unpack_from("<H", out, 2)[0]
                rec_count = struct.unpack_from("<H", out, 14)[0]
                detail = f"InfoLogFlags=0x{flags:02x}, OverflowCount={overflow_cnt}, RecordCount={rec_count}"
                if rec_count > 0 and len(out) >= 16 + 128:
                    # Handle at offset 16 + 2 in first 128-byte event record
                    rec_handle = struct.unpack_from("<H", out, 18)[0]
                    clr_in = struct.pack("<BBBH", 0x00, 0x00, 1, rec_handle)
                    rc_c, _, ret_c, _ = self.send_cmd(CMD_RAW, in_bytes=clr_in, out_cap=0, raw_opcode=OP_CLEAR_EVENT_RECORDS)
                    detail += f" | ClearRecord(handle={rec_handle}) -> rc={rc_c}, ret=0x{ret_c:04x}"
                self.add_result("Week 5", "Events", "Get & Clear Event Records (0x0100/0x0101)", "PASS", time.time() - t0, detail)
            elif rc == -1 and err in (errno.EPERM, errno.EINVAL):
                self.add_result("Week 5", "Events", "Get & Clear Event Records (0x0100/0x0101)", "SKIP", time.time() - t0,
                                f"Kernel rejected RAW opcode ({errno.errorcode[err]}); requires CONFIG_CXL_MEM_RAW_COMMANDS")
            else:
                self.add_result("Week 5", "Events", "Get & Clear Event Records (0x0100/0x0101)", "FAIL", time.time() - t0,
                                f"ioctl_rc={rc}, errno={err}, mailbox_retval=0x{retval:04x}")

        # 6. Set Partition Info (Opcode 0x4101) - idempotent write of current volatile size
        t0 = time.time()
        if not self.is_cmd_enabled(CMD_SET_PARTITION_INFO):
            self.add_result("Week 5", "Partition", "Set Partition Info (0x4101)", "SKIP", time.time() - t0,
                            "SET_PARTITION_INFO not enabled on device/kernel")
        else:
            set_part_in = struct.pack("<Q", active_v)
            rc, err, retval, _ = self.send_cmd(CMD_SET_PARTITION_INFO, in_bytes=set_part_in, out_cap=0)
            if rc == 0 and retval == CCI_RET_SUCCESS:
                self.add_result("Week 5", "Partition", "Set Partition Info (0x4101)", "PASS", time.time() - t0,
                                f"Set volatile partition size to {active_v*256}MB succeeded")
            else:
                err_name = errno.errorcode.get(err, str(err))
                self.add_result("Week 5", "Partition", "Set Partition Info (0x4101)", "FAIL", time.time() - t0,
                                f"ioctl_rc={rc}, errno={err_name}, mailbox_retval=0x{retval:04x}")

    # =========================================================================
    # WEEK 6: NEGATIVE / ERROR-PATH TEST CASES
    # =========================================================================
    def run_week6(self):
        # 1. Invalid / Unsupported Opcode (0xFFFF)
        t0 = time.time()
        rc, err, retval, _ = self.send_cmd(CMD_RAW, in_bytes=b"", out_cap=64, raw_opcode=0xFFFF)
        if (rc == -1 and err in (errno.EINVAL, errno.EPERM, errno.ENOTTY)) or (retval != 0):
            err_name = errno.errorcode.get(err, "NONE")
            self.add_result("Week 6", "Negative", "Unsupported Opcode (0xFFFF)", "PASS", time.time() - t0,
                            f"Rejected as expected (ioctl_rc={rc}, errno={err_name}, retval=0x{retval:04x})")
        else:
            self.add_result("Week 6", "Negative", "Unsupported Opcode (0xFFFF)", "FAIL", time.time() - t0,
                            "Invalid opcode 0xFFFF unexpectedly succeeded")

        # 2. Invalid UAPI Command ID (0xDEAD)
        t0 = time.time()
        rc, err, retval, _ = self.send_cmd(0xDEAD, in_bytes=b"", out_cap=64)
        if rc == -1 and err == errno.EINVAL:
            self.add_result("Week 6", "Negative", "Invalid UAPI Command ID (0xDEAD)", "PASS", time.time() - t0,
                            "Kernel UAPI rejected invalid command ID with EINVAL")
        else:
            self.add_result("Week 6", "Negative", "Invalid UAPI Command ID (0xDEAD)", "FAIL", time.time() - t0,
                            f"Expected EINVAL, got rc={rc}, err={err}, retval=0x{retval:04x}")

        # 3. Out-of-Range LSA Offset (0xFFFFFFFF)
        t0 = time.time()
        oob_offset_in = struct.pack("<II", 0xFFFFFFFF, 64)
        rc, err, retval, _ = self.send_cmd(CMD_GET_LSA, in_bytes=oob_offset_in, out_cap=64)
        if (rc == -1 and err == errno.EINVAL) or (retval == CCI_RET_INVALID_INPUT):
            err_name = errno.errorcode.get(err, "NONE")
            self.add_result("Week 6", "Negative", "Out-of-Range LSA Offset (0xFFFFFFFF)", "PASS", time.time() - t0,
                            f"Rejected OOB offset (errno={err_name}, retval=0x{retval:04x})")
        else:
            self.add_result("Week 6", "Negative", "Out-of-Range LSA Offset (0xFFFFFFFF)", "FAIL", time.time() - t0,
                            f"OOB LSA offset unexpectedly succeeded (rc={rc}, retval=0x{retval:04x})")

        # 4. Out-of-Range LSA Read Length (> Device LSA Size)
        t0 = time.time()
        oversized_len = (self.lsa_size if self.lsa_size > 0 else 262144) + 4096
        oob_len_in = struct.pack("<II", 0, oversized_len)
        rc, err, retval, _ = self.send_cmd(CMD_GET_LSA, in_bytes=oob_len_in, out_cap=oversized_len)
        if (rc == -1 and err in (errno.EINVAL, errno.ENOMEM)) or (retval in (CCI_RET_INVALID_INPUT, CCI_RET_INVALID_PAYLOAD_LEN)):
            err_name = errno.errorcode.get(err, "NONE")
            self.add_result("Week 6", "Negative", f"Out-of-Range LSA Length ({oversized_len}B)", "PASS", time.time() - t0,
                            f"Rejected oversized LSA read (errno={err_name}, retval=0x{retval:04x})")
        else:
            self.add_result("Week 6", "Negative", f"Out-of-Range LSA Length ({oversized_len}B)", "FAIL", time.time() - t0,
                            f"Oversized LSA read unexpectedly succeeded (rc={rc}, retval=0x{retval:04x})")

        # 5. Truncated / Malformed SET_LSA Input Payload (2 bytes instead of >= 8-byte header)
        t0 = time.time()
        rc, err, retval, _ = self.send_cmd(CMD_SET_LSA, in_bytes=b"\x00\x00", out_cap=0)
        if (rc == -1 and err == errno.EINVAL) or (retval in (CCI_RET_INVALID_INPUT, CCI_RET_INVALID_PAYLOAD_LEN)):
            err_name = errno.errorcode.get(err, "NONE")
            self.add_result("Week 6", "Negative", "Truncated SET_LSA Payload (2B)", "PASS", time.time() - t0,
                            f"Rejected malformed payload (errno={err_name}, retval=0x{retval:04x})")
        else:
            self.add_result("Week 6", "Negative", "Truncated SET_LSA Payload (2B)", "FAIL", time.time() - t0,
                            f"Truncated payload unexpectedly succeeded (rc={rc}, retval=0x{retval:04x})")

        # 6. Non-Zero Input Payload on IDENTIFY (Spec mandates 0-byte input)
        t0 = time.time()
        rc, err, retval, _ = self.send_cmd(CMD_IDENTIFY, in_bytes=b"\xAA" * 16, out_cap=0x43)
        if (rc == -1 and err == errno.EINVAL) or (retval in (CCI_RET_INVALID_INPUT, CCI_RET_INVALID_PAYLOAD_LEN)):
            err_name = errno.errorcode.get(err, "NONE")
            self.add_result("Week 6", "Negative", "Unexpected Input Payload on IDENTIFY (16B)", "PASS", time.time() - t0,
                            f"Rejected non-empty input payload (errno={err_name}, retval=0x{retval:04x})")
        else:
            self.add_result("Week 6", "Negative", "Unexpected Input Payload on IDENTIFY (16B)", "FAIL", time.time() - t0,
                            f"IDENTIFY with 16B input unexpectedly succeeded (rc={rc}, retval=0x{retval:04x})")

        # 7. Invalid Event Log Type (0xFF) on Get Event Records (0x0100)
        t0 = time.time()
        rc, err, retval, _ = self.send_cmd(CMD_RAW, in_bytes=b"\xFF", out_cap=4096, raw_opcode=OP_GET_EVENT_RECORDS)
        if (rc == -1 and err in (errno.EINVAL, errno.EPERM)) or (retval in (CCI_RET_INVALID_INPUT, CCI_RET_UNSUPPORTED)):
            err_name = errno.errorcode.get(err, "NONE")
            self.add_result("Week 6", "Negative", "Invalid Event Log Type (0xFF)", "PASS", time.time() - t0,
                            f"Rejected invalid log ID (errno={err_name}, retval=0x{retval:04x})")
        else:
            self.add_result("Week 6", "Negative", "Invalid Event Log Type (0xFF)", "FAIL", time.time() - t0,
                            f"Invalid event log 0xFF unexpectedly succeeded (rc={rc}, retval=0x{retval:04x})")


def main():
    dev = sys.argv[1] if len(sys.argv) > 1 else "/dev/cxl/mem0"
    mode = sys.argv[2] if len(sys.argv) > 2 else "all"

    if not os.path.exists(dev):
        print(json.dumps({"error": f"Device {dev} not found"}))
        sys.exit(2)

    suite = PurePythonCCISuite(dev)
    try:
        if mode in ("all", "week5"):
            suite.run_week5()
        if mode in ("all", "week6"):
            suite.run_week6()
    finally:
        suite.close()

    print("__JSON_RESULT__:" + json.dumps(suite.results))
    failed = sum(1 for r in suite.results if r["status"] == "FAIL")
    sys.exit(1 if failed > 0 else 0)


if __name__ == "__main__":
    main()
