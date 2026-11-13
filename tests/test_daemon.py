import os
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import tempfile
import time
import unittest


ROOT = Path(__file__).resolve().parents[1]
MAGIC = 0x4B414C56
FORMATS = {
    "S16_LE": (2, b"\x34\x12", 0x1234, 12, 0xFFFF),
    "S24_3LE": (3, b"\x56\x34\x12", 0x123456, 4, 0xFFFFFF),
}


@unittest.skipUnless(shutil.which("aplay"), "aplay is required")
class KeepaliveDaemonTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp_root = tempfile.TemporaryDirectory(prefix="keepalive-test-")
        cls.shim = Path(cls.temp_root.name) / "clocked_pcm.so"
        subprocess.run(
            [
                "cc", "-shared", "-fPIC", "-Wall", "-Wextra", "-Werror",
                "-o", str(cls.shim), str(ROOT / "tests" / "clocked_pcm.c"), "-ldl",
            ],
            check=True,
        )

    @classmethod
    def tearDownClass(cls):
        cls.temp_root.cleanup()

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="keepalive-case-")
        self.directory = Path(self.temp.name)
        self.socket_path = self.directory / "control.sock"
        self.output_path = self.directory / "output.raw"
        self.config_path = self.directory / "asound.conf"
        self.log_path = self.directory / "daemon.log"
        self.daemon = None
        self.iec958 = False

    def tearDown(self):
        self.stop_daemon()
        self.temp.cleanup()

    def stop_daemon(self):
        if self.daemon and self.daemon.poll() is None:
            self.daemon.terminate()
            self.daemon.wait(timeout=3)
        self.daemon = None

    def start_daemon(self, partial_writes=False, iec958=False):
        self.iec958 = iec958
        capture = (
            f'type file slave.pcm {{ type null }} file "{self.output_path}" '
            'format "raw"'
        )
        sink = (
            f"type iec958 slave.pcm {{ {capture} }} "
            "slave.format IEC958_SUBFRAME_LE"
            if iec958 else capture
        )
        self.config_path.write_text(
            # Loaded by path, as the plugin's ALSA contribution loads it.
            f'pcm_type.keepalive {{ lib "{ROOT / "libasound_module_pcm_keepalive.so"}" }}\n'
            f'pcm.probe {{ type keepalive socket "{self.socket_path}" }}\n'
            f"pcm.sink {{ {sink} }}\n",
            encoding="utf-8",
        )
        env = dict(os.environ)
        env.update({
            "ALSA_CONFIG_PATH": str(self.config_path),
            "LD_PRELOAD": str(self.shim),
        })
        if partial_writes:
            env["KEEPALIVE_TEST_PARTIAL_WRITES"] = "1"

        log = self.log_path.open("w", encoding="utf-8")
        self.daemon = subprocess.Popen(
            [
                str(ROOT / "audio-keepalive-daemon"),
                "--pcm", "sink",
                "--fallback-pcm", "sink",
                "--socket", str(self.socket_path),
            ],
            env=env,
            stdout=log,
            stderr=log,
        )
        log.close()
        for _ in range(300):
            if self.socket_path.exists():
                return
            if self.daemon.poll() is not None:
                self.fail(self.log_path.read_text(encoding="utf-8"))
            time.sleep(0.01)
        self.fail("daemon did not create its control socket")

    def play(self, sample_format, duration_seconds=1, data=None):
        channels = 2
        bytes_per_sample, sample, target, shift, mask = FORMATS[sample_format]
        frame_count = 44100 * duration_seconds
        input_path = self.directory / "input.raw"
        input_path.write_bytes(
            data if data is not None else sample * frame_count * channels)

        env = dict(os.environ)
        env.update({
            "ALSA_CONFIG_PATH": str(self.config_path),
        })
        result = subprocess.run(
            [
                "aplay", "-q", "-D", "probe", "-t", "raw", "-f", sample_format,
                "-c", str(channels), "-r", "44100", "--period-size=5512",
                "--buffer-size=22048", str(input_path),
            ],
            env=env,
            capture_output=True,
            text=True,
            timeout=30,
        )
        self.assertEqual(result.returncode, 0, result.stderr)

    def assert_samples(self, sample_format, duration_seconds=1):
        channels = 2
        bytes_per_sample, _, target, shift, mask = FORMATS[sample_format]
        frame_count = 44100 * duration_seconds
        data = self.output_path.read_bytes()
        if self.iec958:
            sample_count = len(data) // 4
            samples = struct.unpack(f"<{sample_count}I", data[:sample_count * 4])
            samples = [(value >> shift) & mask for value in samples]
        else:
            sample_count = len(data) // bytes_per_sample
            samples = [
                int.from_bytes(
                    data[index * bytes_per_sample:(index + 1) * bytes_per_sample],
                    "little",
                    signed=True,
                )
                for index in range(sample_count)
            ]
        heard = sum(value == target for value in samples)
        self.assertEqual(heard, frame_count * channels)

    def test_drain_preserves_pcm_samples_for_hdmi_formats(self):
        for sample_format in FORMATS:
            with self.subTest(format=sample_format):
                self.stop_daemon()
                self.output_path.unlink(missing_ok=True)
                self.start_daemon(iec958=True)
                self.play(sample_format)
                self.stop_daemon()
                self.assert_samples(sample_format)

    def test_partial_alsa_writes_are_retried_without_losing_samples(self):
        self.start_daemon(partial_writes=True, iec958=True)
        self.play("S16_LE")
        self.stop_daemon()
        self.assert_samples("S16_LE")

    def test_music_reaches_output_contiguously(self):
        # Idle noise spliced between music periods would play as dropouts
        # while the sample count still matches.
        music = os.urandom(44100 * 2 * 2 * 2)
        self.start_daemon()
        self.play("S16_LE", data=music)
        self.stop_daemon()
        output = self.output_path.read_bytes()
        start = output.find(music[:64])
        self.assertGreaterEqual(start, 0, "music start not found in output")
        self.assertEqual(start % 4, 0)
        received = output[start:start + len(music)]
        mismatch = next(
            (i for i in range(0, len(music), 4)
             if received[i:i + 4] != music[i:i + 4]),
            None,
        )
        self.assertIsNone(
            mismatch, f"music interrupted after {mismatch} bytes")

    def test_fragmented_client_message_does_not_block_idle_keepalive(self):
        self.start_daemon()
        for _ in range(100):
            if self.output_path.exists() and self.output_path.stat().st_size >= 4096:
                break
            time.sleep(0.01)
        self.assertTrue(self.output_path.exists())
        before = self.output_path.stat().st_size

        with socket.socket(socket.AF_UNIX) as client:
            client.connect(str(self.socket_path))
            client.sendall(struct.pack("=I", MAGIC)[:1])
            time.sleep(0.25)

        after = self.output_path.stat().st_size
        self.assertGreater(after, before)


if __name__ == "__main__":
    unittest.main()
