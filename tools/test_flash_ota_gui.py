"""Headless regression tests for the GUI OTA state machine.

The transport below models the RP2040's five-byte sequence filter and its
256-byte page commit ACK.  No HID device or Qt window is required.
"""
import importlib.util
import struct
import tempfile
import unittest
import zlib
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("flash_ota_gui", ROOT / "tools" / "flash_ota_gui.py")
gui = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(gui)


class _Signal:
    def __init__(self):
        self.values = []
    def emit(self, *args):
        self.values.append(args)


class _FastLimiter:
    rates = []
    def __init__(self, *args, **kwargs):
        self.rates.append(args[0] if args else kwargs.get("rate_bytes_per_sec"))
    def wait_for(self, payload_bytes):
        pass


class FakeDevice:
    def __init__(self, payload, drop_index=None, boot_ok=True, crc=None):
        self.payload = payload
        self.drop_index = drop_index
        self.boot_ok = boot_ok
        self.reported_crc = zlib.crc32(payload) & 0xffffffff if crc is None else crc
        self.accepted = bytearray()
        self.expected_seq = 1
        self.dropped = False
        self.statuses = []
        self.cached = (gui.DFU_STATUS_IDLE, 0, 0, 0, 0)
        self.token = 0
        self.session = 0
        self.activated = False

    def set_feature(self, _handle, command):
        cmd, self.session = command[0], command[1]
        self.token += 1
        if cmd == gui.DFU_CMD_START:
            self.accepted.clear(); self.expected_seq = 1
            self.cached = (gui.DFU_STATUS_OK, self.session, self.token, 0, 0)
        elif cmd == gui.DFU_CMD_CRC:
            self.cached = (gui.DFU_STATUS_OK, self.session, self.token, 0, 0)
        elif cmd == gui.DFU_CMD_DATA:
            seq = command[7]
            index = (seq - 1) & 0xff
            chunk = bytes(command[2:7])
            if self.drop_index is not None and index == self.drop_index and not self.dropped:
                self.dropped = True
            elif seq == self.expected_seq:
                before = len(self.accepted)
                self.accepted.extend(chunk[:max(0, min(5, len(self.payload) - before))])
                self.expected_seq = (self.expected_seq + 1) & 0xff
                after = len(self.accepted)
                for page in range((before // 256) + 1, (after // 256) + 1):
                    self.statuses.append((gui.DFU_STATUS_OK, self.session, self.token, 0, page * 256))
                if after == len(self.payload):
                    self.statuses.append((gui.DFU_STATUS_OK, self.session, self.token, 0, after))
            else:
                self.cached = (gui.DFU_STATUS_OK, self.session, self.token, 0, len(self.accepted))
        elif cmd == gui.DFU_CMD_FINISH:
            self.statuses.append((gui.DFU_STATUS_OK, self.session, self.token, 0, len(self.accepted)))
            self.statuses.append((gui.DFU_STATUS_VERIFIED, self.session, self.token, 0, self.reported_crc))
        elif cmd == gui.DFU_CMD_ACTIVATE:
            self.activated = True
            if self.boot_ok:
                self.statuses.append((gui.DFU_STATUS_APPLYING, self.session, self.token, 0, 0))
                self.statuses.append((gui.DFU_STATUS_BOOT_OK, self.session, self.token, 0, self.reported_crc))
        return True

    def get_status(self, _handle):
        if self.statuses:
            self.cached = self.statuses.pop(0)
        return self.cached


class GuiOtaRegressionTests(unittest.TestCase):
    def test_strict_rate_limiter_charges_retries_and_stalls(self):
        now = [0.0]
        sleeps = []
        clock = lambda: now[0]
        def sleep(duration):
            sleeps.append(duration)
            now[0] += duration
        limiter = gui.StrictRateLimiter(gui.DATA_RATE_BYTES_PER_SEC, clock=clock, sleeper=sleep)
        for _ in range(3):
            limiter.wait_for(5)  # initial DATA, failed write retry, next DATA
        now[0] += 20.0  # an idle stall cannot create catch-up credit
        limiter.wait_for(5)
        self.assertEqual(len(sleeps), 4)
        self.assertTrue(all(duration >= 5.0 / gui.DATA_RATE_BYTES_PER_SEC - 1e-9 for duration in sleeps))
        self.assertLessEqual(20.0 / (now[0] - 0.0), gui.DATA_RATE_BYTES_PER_SEC)

    def test_worker_uses_configured_data_rate(self):
        _FastLimiter.rates.clear()
        self.run_worker(b"firmware")
        self.assertEqual(_FastLimiter.rates, [gui.DATA_RATE_BYTES_PER_SEC])

    def run_worker(self, payload, **kwargs):
        device = FakeDevice(payload, **kwargs)
        old = {name: getattr(gui, name) for name in ("open_receiver", "set_feature", "get_status", "StrictRateLimiter")}
        gui.open_receiver = lambda: object()
        gui.set_feature = device.set_feature
        gui.get_status = device.get_status
        gui.StrictRateLimiter = _FastLimiter
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "test.wkota"
            crc = zlib.crc32(payload) & 0xffffffff
            header = struct.pack("<8sHBBHHIIII", b"WKRPOTA1", 1, 1, 1,
                                 gui.OTA_BOARD_WEACT_RP2040_4MB, 32,
                                 len(payload), crc, 0, 0)
            path.write_bytes(header + payload)
            worker = gui.FlasherWorker(str(path))
            worker.sig_log = _Signal(); worker.sig_status = _Signal()
            worker.sig_progress = _Signal(); worker.sig_finished = _Signal()
            worker.run()
        for name, value in old.items():
            setattr(gui, name, value)
        return device, worker.sig_finished.values, worker.sig_log.values

    def test_page_ack_256_and_final_partial(self):
        payload = bytes((i * 7) & 0xff for i in range(1603))
        device, finished, _ = self.run_worker(payload)
        self.assertEqual(bytes(device.accepted), payload)
        self.assertTrue(device.activated)
        self.assertTrue(finished[-1][0])

    def test_lost_middle_chunk_recovers_without_corruption(self):
        payload = bytes((i * 17) & 0xff for i in range(1603))
        device, finished, _ = self.run_worker(payload, drop_index=17)
        self.assertEqual(bytes(device.accepted), payload)
        self.assertTrue(device.activated)
        self.assertTrue(finished[-1][0])

    def test_crc_failure_and_missing_boot_are_failures(self):
        payload = b"firmware" * 40
        bad, bad_finished, _ = self.run_worker(payload, crc=0x12345678)
        self.assertFalse(bad.activated)
        self.assertFalse(bad_finished[-1][0])
        missing, missing_finished, _ = self.run_worker(payload, boot_ok=False)
        self.assertTrue(missing.activated)
        self.assertFalse(missing_finished[-1][0])


if __name__ == "__main__":
    unittest.main()
