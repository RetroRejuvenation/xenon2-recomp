# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
#
# Tests for the Atari ST tools in tools/acq (sttrace.py, stshape.py, stcopylock.py, ymrender.py, stemu.c). Every fixture is synthetic
# (generated here); no game bytes are involved. Tests that need libcapstone, numpy or a built Musashi skip themselves.
#   python3 -m unittest discover -s tools/acq -p 'test_*.py'
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, ".."))

try:
    import dis68k
    dis68k.Dis()
    HAVE_CAPSTONE = True
except Exception:  # libcapstone missing
    HAVE_CAPSTONE = False
try:
    import numpy  # noqa: F401
    HAVE_NUMPY = True
except Exception:
    HAVE_NUMPY = False


def trace_encrypt(instructions, base, prefix):
    """Encrypt a list of plain instruction byte strings the way the ST trace-vector loader decrypts them: the first
    longword of each instruction is XORed with swap(~raw longword just before it). Returns (raw image, addresses)."""
    mem = bytearray(prefix)
    addrs = []
    for ins in instructions:
        a = base + len(mem)  # address of this instruction
        addrs.append(a)
        mem += bytes(max(4, len(ins)))
        prev = int.from_bytes(mem[len(mem) - max(4, len(ins)) - 4:len(mem) - max(4, len(ins))], "big")
        key = (~prev) & 0xFFFFFFFF
        key = ((key << 16) | (key >> 16)) & 0xFFFFFFFF
        kb = key.to_bytes(4, "big")
        o = len(mem) - max(4, len(ins))
        for k in range(min(4, len(ins))):
            mem[o + k] = ins[k] ^ kb[k]
        for k in range(4, len(ins)):
            mem[o + k] = ins[k]
        # a 2-byte instruction leaves the next two bytes to the next instruction: trim the padding so it overlaps
        if len(ins) == 2:
            del mem[-2:]
    mem += bytes(16)
    return bytes(mem), addrs


@unittest.skipUnless(HAVE_CAPSTONE, "libcapstone not available")
class SttraceTests(unittest.TestCase):
    def test_plain_long_reverses_the_encryption(self):
        import sttrace
        base = 0x40000
        prog = [bytes.fromhex("7001"), bytes.fromhex("7202"), bytes.fromhex("d081"),
                bytes.fromhex("41f912345678"), bytes.fromhex("46fc2700"), bytes.fromhex("4e75")]
        mem, addrs = trace_encrypt(prog, base, bytes.fromhex("deadbeef"))
        for a, ins in zip(addrs, prog):
            got = sttrace.plain_long(mem, base, a)
            self.assertEqual(got[:min(4, len(ins))], ins[:4], hex(a))

    def test_follow_decodes_the_straight_line_and_the_branch(self):
        import sttrace
        base = 0x40000
        # moveq #1,d0 ; moveq #2,d1 ; bra.w +6 (skips a 2-byte junk word) ; add.l d1,d0 ; rts
        prog = [bytes.fromhex("7001"), bytes.fromhex("7202"), bytes.fromhex("60000004"), bytes.fromhex("4e71"),
                bytes.fromhex("d081"), bytes.fromhex("4e75")]
        mem, addrs = trace_encrypt(prog, base, bytes.fromhex("12345678"))
        res = sttrace.follow(mem, base, addrs[0])
        mn = [res[a][2] for a in sorted(res)]
        self.assertEqual(mn[:3], ["moveq", "moveq", "bra.w"])
        self.assertTrue(any(x.startswith("add") for x in mn))
        self.assertEqual(mn[-1], "rts")
        self.assertNotIn(addrs[3], res)  # the skipped word is never executed

    def test_first_longword_key_depends_on_the_preceding_bytes(self):
        import sttrace
        mem = bytes.fromhex("0000000000000000") + bytes(16)
        mem2 = bytes.fromhex("0000000100000000") + bytes(16)
        self.assertNotEqual(sttrace.plain_long(mem, 0, 4), sttrace.plain_long(mem2, 0, 4))


@unittest.skipUnless(HAVE_CAPSTONE, "libcapstone not available")
class StshapeTests(unittest.TestCase):
    CODE_A = bytes.fromhex("303c1234" "33fc0007" "00ff8240" "7000" "4ef9" "00040100" "d081" "2f08" "41f9" "00ff8800" "1010" "6700fff0")
    CODE_B = bytes.fromhex("303c9999" "33fc0001" "00dff180" "7000" "4ef9" "00020000" "d081" "2f08" "41f9" "00dff096" "1010" "6700ffe0")

    def test_shape_ignores_numbers_and_absolute_sizes(self):
        import stshape
        self.assertEqual(stshape.shape("move.w", "#$1234, $ff8240.l"), stshape.shape("move.w", "#$1, $dff180.w"))
        self.assertNotEqual(stshape.shape("move.w", "d0, d1"), stshape.shape("move.l", "d0, d1"))

    def test_same_code_with_other_constants_matches_fully(self):
        import stshape
        r = stshape.compare("a", self.CODE_A * 4, "b", self.CODE_B * 4, n=4)
        self.assertGreater(r["a_in_b"][0], 0.99)
        self.assertGreater(r["b_in_a"][0], 0.99)

    def test_unrelated_data_matches_little(self):
        import stshape
        import random
        rnd = random.Random(5)
        noise = bytes(rnd.randrange(256) for _ in range(4000))
        r = stshape.compare("a", self.CODE_A * 40, "b", noise, n=6)
        self.assertLess(r["a_in_b"][0], 0.05)

    def test_byte_overlap_finds_a_shared_table(self):
        import stshape
        table = bytes((i * 37 + 11) & 0xFF for i in range(200))
        a = bytes(100) + table + bytes(100)
        b = bytes(50) + table + bytes(300)
        frac, hit, total = stshape.byte_overlap(a, b, 48)
        self.assertGreater(hit, 50)
        self.assertGreater(frac, 0.5)


@unittest.skipUnless(HAVE_NUMPY, "numpy not available")
class YmrenderTests(unittest.TestCase):
    IPS = 4_500_000

    def _peak_hz(self, samples, rate=44100):
        import numpy as np
        spec = np.abs(np.fft.rfft(samples[:rate].astype(float)))
        return int(np.argmax(spec[1:]) + 1)

    def test_a_440_hz_tone(self):
        import ymrender
        period = round(2_000_000 / (16 * 440))
        ev = [(0, 0, period & 255), (0, 1, period >> 8), (0, 7, 0xFE), (0, 8, 15), (self.IPS, 8, 15)]
        s = ymrender.render(ev, self.IPS)
        self.assertAlmostEqual(self._peak_hz(s), 440, delta=3)

    def test_disabled_channel_is_silent_and_volume_zero_is_silent(self):
        import ymrender
        ev = [(0, 0, 100), (0, 1, 0), (0, 7, 0xFF), (0, 8, 15), (self.IPS // 2, 8, 15)]
        self.assertLess(abs(ymrender.render(ev, self.IPS)).max(), 50)
        ev = [(0, 0, 100), (0, 1, 0), (0, 7, 0xFE), (0, 8, 0), (self.IPS // 2, 8, 0)]
        self.assertLess(abs(ymrender.render(ev, self.IPS)).max(), 50)

    def test_noise_is_broadband(self):
        import numpy as np
        import ymrender
        ev = [(0, 6, 4), (0, 7, 0xF7), (0, 8, 15), (self.IPS, 8, 15)]
        s = ymrender.render(ev, self.IPS)
        spec = np.abs(np.fft.rfft(s[:44100].astype(float)))
        self.assertLess(spec.max() / spec.mean(), 40)  # no single dominant line

    def test_envelope_shape_changes_the_level_over_time(self):
        import ymrender
        import numpy as np
        ev = [(0, 0, 80), (0, 1, 0), (0, 7, 0xFE), (0, 8, 0x10), (0, 11, 200), (0, 12, 0), (0, 13, 0), (self.IPS, 13, 0)]
        s = ymrender.render(ev, self.IPS).astype(float)
        loud = np.abs(s[:2000]).mean()
        quiet = np.abs(s[-4000:-2000]).mean()
        self.assertGreater(loud, 4 * quiet)  # shape 0: one falling ramp, then silence

    def test_read_log_parses_and_sorts(self):
        import ymrender
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "ym.log")
            with open(p, "w") as f:
                f.write("20 7 255\n10 0 1\nnot a line\n")
            self.assertEqual(ymrender.read_log(p), [(10, 0, 1), (20, 7, 255)])


class StcopylockTests(unittest.TestCase):
    """The Copylock ST layer model, on synthetic data only."""

    def test_key_is_six_add_rotate_rounds(self):
        import stcopylock
        sector = bytes(range(1, 25)) + bytes(488)
        d0 = 0
        for i in range(6):
            lw = int.from_bytes(sector[4 * i:4 * i + 4], "big")
            d0 = (d0 + lw) & 0xFFFFFFFF
            d0 = ((d0 << 3) | (d0 >> 29)) & 0xFFFFFFFF
        self.assertEqual(stcopylock.key_from_sector(sector), d0)
        self.assertEqual(stcopylock.key_from_sector(bytes(24)), 0)
        with self.assertRaises(ValueError):
            stcopylock.key_from_sector(bytes(23))

    def test_signature_check(self):
        import stcopylock
        self.assertTrue(stcopylock.has_signature(b"Rob Northen Comp" + bytes(16)))
        self.assertFalse(stcopylock.has_signature(b"Rob Northen Comq" + bytes(16)))
        self.assertFalse(stcopylock.has_signature(b"Rob"))

    def test_handler_addresses_come_from_the_key(self):
        import stcopylock
        key = 0x12345678
        dummy = 0xA5A5A5A5
        a = stcopylock.handler_address(key, dummy)
        self.assertEqual(a ^ dummy, key)
        self.assertEqual(stcopylock.handler_address(dummy ^ 0x67CFE, dummy), 0x67CFE)

    def test_prefix_xor_matches_the_loop_and_inverts(self):
        import stcopylock
        src = bytes((i * 37 + 5) & 0xFF for i in range(300))
        out = stcopylock.prefix_xor(src, 0x5A)
        acc = 0x5A
        for i, b in enumerate(src):
            acc ^= b
            self.assertEqual(out[i], acc)
        back = bytes([out[0] ^ 0x5A]) + bytes(out[i] ^ out[i - 1] for i in range(1, len(out)))
        self.assertEqual(back, src)
        self.assertEqual(stcopylock.prefix_xor(b"", 1), b"")

    def test_chain_xor_in_place_from_an_offset(self):
        import stcopylock
        buf = bytes((i * 11 + 3) & 0xFF for i in range(64))
        out = stcopylock.chain_xor_in_place(buf, 8)
        self.assertEqual(out[:9], buf[:9])
        for i in range(8, 63):
            self.assertEqual(out[i + 1], buf[i + 1] ^ out[i])
        # an empty or one-byte buffer does not fail
        self.assertEqual(stcopylock.chain_xor_in_place(b"\x07", 0), b"\x07")
        self.assertEqual(stcopylock.chain_xor_in_place(b"", 0), b"")

    def test_layer1_decrypt_range_and_seed(self):
        import stcopylock
        stage1 = bytes(0x28000)
        key = 0x12345678
        body = stcopylock.layer1_decrypt(stage1, key)
        self.assertEqual(len(body), stcopylock.LAYER1_END - stcopylock.LAYER1_SRC)
        self.assertEqual(set(body), {(key & 0xFF) ^ stcopylock.LAYER1_INIT_XOR})
        with self.assertRaises(ValueError):
            stcopylock.layer1_decrypt(bytes(100), key)

    def test_trace_plain_matches_the_test_encryptor(self):
        import stcopylock
        ins = bytes.fromhex("23c000002000")
        prefix = bytes.fromhex("11223344")
        raw, addrs = trace_encrypt([ins], 0x40000, prefix)
        o = addrs[0] - 0x40000
        raw_long = int.from_bytes(raw[o:o + 4], "big")
        prev = int.from_bytes(raw[o - 4:o], "big")
        self.assertEqual(stcopylock.trace_plain_first_long(raw_long, prev).to_bytes(4, "big"), ins[:4])


class StemuTests(unittest.TestCase):
    """Builds the analysis harness when a Musashi build tree exists and runs a tiny synthetic boot sector."""

    @classmethod
    def setUpClass(cls):
        cls.gen = None
        for cand in (os.path.join(ROOT, "build", "musashi_gen"), os.environ.get("X2_MUSASHI_GEN", "")):
            if cand and os.path.exists(os.path.join(cand, "m68kops.c")):
                cls.gen = cand
        cls.cc = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
        cls.tmp = tempfile.mkdtemp()
        cls.exe = None
        if cls.gen and cls.cc:
            exe = os.path.join(cls.tmp, "stemu")
            cmd = [cls.cc, "-O1", "-w", "-DMUSASHI_CNF=\"st_m68kconf.h\"", "-I" + HERE, "-I" + os.path.join(ROOT, "vendor", "musashi"),
                   "-I" + cls.gen, os.path.join(HERE, "stemu.c"), os.path.join(ROOT, "vendor", "musashi", "m68kcpu.c"),
                   os.path.join(cls.gen, "m68kops.c"), os.path.join(ROOT, "runtime", "musashi_no_fpu.c"), "-o", exe]
            if subprocess.run(cmd, capture_output=True).returncode == 0:
                cls.exe = exe

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def _disk(self, boot_code):
        boot = bytearray(512)
        boot[:len(boot_code)] = boot_code
        return bytes(boot) + bytes(512 * 9)  # a one-track image

    def test_runs_a_boot_sector_and_dumps_memory(self):
        if not self.exe:
            self.skipTest("no Musashi build tree or C compiler")
        # moveq #7,d0 ; move.l d0,$2000.l ; bra.b * (at boot+8)
        code = bytes.fromhex("7007" "23c000002000" "60fe")
        disk = os.path.join(self.tmp, "t.st")
        with open(disk, "wb") as f:
            f.write(self._disk(code))
        out = os.path.join(self.tmp, "mem.bin")
        r = subprocess.run([self.exe, disk, "--boot-at", "0x10000", "--stop", "0x10008", "--dump", out + ":0x2000:4"],
                           capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        with open(out, "rb") as f:
            self.assertEqual(f.read(), bytes.fromhex("00000007"))

    def test_the_fdc_reads_a_sector_into_dma_memory(self):
        if not self.exe:
            self.skipTest("no Musashi build tree or C compiler")
        # DMA address $3000, one sector, sector register 3, read sector, then poll the MFP bit the controller drops when done
        code = bytes.fromhex(
            "13fc0000" "00ff8609"   # move.b #0,$ff8609
            "13fc0030" "00ff860b"   # move.b #$30,$ff860b
            "13fc0000" "00ff860d"   # move.b #0,$ff860d
            "33fc0090" "00ff8606"   # move.w #$90,$ff8606  (sector count register)
            "33fc0001" "00ff8604"   # move.w #1,$ff8604
            "33fc0084" "00ff8606"   # move.w #$84,$ff8606  (sector register)
            "33fc0003" "00ff8604"   # move.w #3,$ff8604
            "33fc0080" "00ff8606"   # move.w #$80,$ff8606  (command register)
            "33fc0080" "00ff8604"   # move.w #$80,$ff8604  (read sector)
            "08390005" "00fffa01"   # btst.b #5,$fffa01   (still busy: bit 5 high)
            "66f6"                  # bne.b back to the btst
            "60fe")                 # bra.b *  (the stop address)
        data = bytearray(self._disk(code))
        data[2 * 512:3 * 512] = bytes([0xA5]) * 512      # sector 3 of track 0
        disk = os.path.join(self.tmp, "t2.st")
        with open(disk, "wb") as f:
            f.write(bytes(data))
        out = os.path.join(self.tmp, "mem2.bin")
        stop_pc = 0x10000 + len(code) - 2
        r = subprocess.run([self.exe, disk, "--boot-at", "0x10000", "--max", "200000", "--stop", hex(stop_pc), "--dump", out + ":0x3000:512"],
                           capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        with open(out, "rb") as f:
            self.assertEqual(f.read(), bytes([0xA5]) * 512)


    def _run(self, name, code, extra, dump=None, env=None):
        disk = os.path.join(self.tmp, name + ".st")
        with open(disk, "wb") as f:
            f.write(self._disk(code))
        cmd = [self.exe, disk, "--boot-at", "0x10000"] + extra
        if dump:
            cmd += ["--dump", os.path.join(self.tmp, name + ".bin") + ":" + dump]
        r = subprocess.run(cmd, capture_output=True, text=True, env=dict(os.environ, **(env or {})))
        out = None
        if dump:
            with open(os.path.join(self.tmp, name + ".bin"), "rb") as f:
                out = f.read()
        return r, out

    def test_reg_option_presets_a_register(self):
        if not self.exe:
            self.skipTest("no Musashi build tree or C compiler")
        # move.l d3,$2000.l ; bra.b *
        code = bytes.fromhex("23c300002000" "60fe")
        r, out = self._run("reg", code, ["--reg", "d3=0xCAFEF00D", "--stop", "0x10006"], "0x2000:4")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(out, bytes.fromhex("cafef00d"))

    def test_set32_probe_writes_once_the_pc_is_reached(self):
        if not self.exe:
            self.skipTest("no Musashi build tree or C compiler")
        # moveq #0,d0 ; move.l $2000.l,$2004.l ; bra.b *   with the probe writing $11223344 to $2000 at the move
        code = bytes.fromhex("7000" "23f900002000" "00002004" "60fe")
        r, out = self._run("set32", code, ["--set32", "0x10002:0x2000:0x11223344", "--stop", "0x1000c"], "0x2000:8")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(out, bytes.fromhex("11223344" "11223344"))

    def test_max_stops_at_the_exact_instruction_count(self):
        if not self.exe:
            self.skipTest("no Musashi build tree or C compiler")
        code = bytes.fromhex("60fe")  # bra.b *
        r, _ = self._run("max", code, ["--max", "123"])
        self.assertEqual(r.returncode, 3, r.stderr)  # 3 = ran out of instructions without reaching a stop address
        self.assertIn("insns=123 ", r.stderr)

    def test_watch_logs_writes_in_the_range(self):
        if not self.exe:
            self.skipTest("no Musashi build tree or C compiler")
        # moveq #7,d0 ; move.l d0,$2000.l ; bra.b *
        code = bytes.fromhex("7007" "23c000002000" "60fe")
        r, _ = self._run("watch", code, ["--watch", "0x2000:0x2010", "--stop", "0x10008"])
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("2000", r.stderr)


if __name__ == "__main__":
    unittest.main()
