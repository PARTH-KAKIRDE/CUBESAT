"""CM4 side of the ICD I2C link (master).

Constants come from ../icd.h at import time, so the C header stays the single
source of truth. Uses only the standard library (Linux i2c-dev).
"""
import errno
import fcntl
import os
import re
import time

ICD_H = os.environ.get("ICD_H", os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                              "..", "icd.h"))


def load_icd(path=ICD_H):
    """Return {NAME: int} for every '#define ICD_NAME <int>' in the header."""
    defs = {}
    pat = re.compile(r"^\s*#define\s+ICD_(\w+)\s+(0[xX][0-9A-Fa-f]+|\d+)\b")
    with open(path) as f:
        for line in f:
            m = pat.match(line)
            if m:
                defs[m.group(1)] = int(m.group(2), 0)
    return defs


ICD = load_icd()
STATUS_NAMES = {v: k[3:] for k, v in ICD.items() if k.startswith("ST_")}
I2C_SLAVE = 0x0703      # ioctl from <linux/i2c-dev.h>


def crc16(data, crc=None):
    """CRC-16/CCITT-FALSE. crc16(b"123456789") == 0x29B1."""
    crc = ICD["CRC_INIT"] if crc is None else crc
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ ICD["CRC_POLY"]) if crc & 0x8000 else (crc << 1)
            crc &= 0xFFFF
    return crc


def crc_selftest():
    got = crc16(b"123456789")
    if got != ICD["CRC_CHECK"]:
        raise AssertionError(f"CRC self-test failed: 0x{got:04X} != 0x{ICD['CRC_CHECK']:04X}")


class LinkError(Exception):
    """A transaction failed (I/O error, bad frame, or still BUSY after retries)."""


def build_cmd(cmd, payload=b""):
    if len(payload) > ICD["MAX_PAYLOAD"]:
        raise ValueError("payload too long")
    body = bytes([cmd, len(payload)]) + bytes(payload)
    c = crc16(body)
    return body + bytes([c & 0xFF, c >> 8])


def parse_resp(resp, cmd):
    """Check a 32-byte response; return (status, payload) or raise LinkError."""
    if len(resp) != ICD["RESP_LEN"]:
        raise LinkError(f"short read: {len(resp)} bytes")
    n = resp[ICD["RESP_OFS_LEN"]]
    if n > ICD["MAX_PAYLOAD"]:
        raise LinkError(f"bad LEN {n}")
    end = ICD["RESP_HDR_LEN"] + n
    got = resp[end] | (resp[end + 1] << 8)
    if crc16(resp[:end]) != got:
        raise LinkError("response CRC mismatch")
    if resp[ICD["RESP_OFS_ID"]] != cmd:
        raise LinkError(f"echo 0x{resp[0]:02X} != cmd 0x{cmd:02X}")
    return resp[ICD["RESP_OFS_STATUS"]], bytes(resp[ICD["RESP_OFS_PAYLOAD"]:end])


class Slave:
    """One STM32 slave on a CM4 I2C bus."""

    def __init__(self, bus, addr):
        self.addr = addr
        self.fd = os.open(f"/dev/i2c-{bus}", os.O_RDWR)
        fcntl.ioctl(self.fd, I2C_SLAVE, addr)

    def close(self):
        os.close(self.fd)

    def transact_raw(self, frame, cmd):
        """Write one command frame, wait, read the response. Never re-sends the
        command, so a retry can't run an actuator command twice."""
        try:
            os.write(self.fd, bytes(frame))
            time.sleep(ICD["TURNAROUND_MS"] / 1000)
            for attempt in range(ICD["BUSY_RETRIES"] + 1):
                status, payload = parse_resp(os.read(self.fd, ICD["RESP_LEN"]), cmd)
                if status != ICD["ST_BUSY"]:
                    return status, payload
                time.sleep(ICD["BUSY_RETRY_MS"] / 1000)
        except OSError as e:
            raise LinkError(f"I/O error: {errno.errorcode.get(e.errno, e.errno)}") from e
        raise LinkError("still BUSY after retries")

    def transact(self, cmd, payload=b""):
        return self.transact_raw(build_cmd(cmd, payload), cmd)


class Monitor:
    """COMM_FAULT tracking: N consecutive failed transactions -> fault, keep polling."""

    def __init__(self, name, log=print):
        self.name, self.log = name, log
        self.fails = 0
        self.comm_fault = False

    def ok(self):
        if self.comm_fault:
            self.log(f"{self.name}: link recovered")
        self.fails, self.comm_fault = 0, False

    def failed(self, err):
        self.fails += 1
        if self.fails >= ICD["COMM_FAULT_AFTER"] and not self.comm_fault:
            self.comm_fault = True
            self.log(f"{self.name}: COMM_FAULT after {self.fails} failures ({err})")


def bus_speed_hz(bus):
    """Configured clock of /dev/i2c-<bus> from the device tree, or None."""
    p = f"/sys/class/i2c-adapter/i2c-{bus}/of_node/clock-frequency"
    try:
        with open(p, "rb") as f:
            return int.from_bytes(f.read(4), "big")
    except OSError:
        return None
