#!/usr/bin/env python3
"""Real sockets, timed MP3/AAC fixtures and decoded audio; no external services."""
import array
import http.server
import json
import math
import os
from pathlib import Path
import signal
import socket
import subprocess
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
BIN = Path(os.environ.get("PERMASTREAM_BIN", ROOT / "permastream"))


def run(*args, **kwargs):
    return subprocess.run(args, check=True, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, **kwargs).stdout


def wait_for(predicate, timeout=12, message="condition"):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.05)
    raise AssertionError(f"Timed out waiting for {message}")


def mp3_frames(data, complete=True):
    result = []
    offset = 0
    rates = [0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320]
    while offset + 4 <= len(data):
        h = int.from_bytes(data[offset:offset + 4], "big")
        assert h & 0xffe00000 == 0xffe00000, f"MP3 sync lost at {offset}"
        assert (h >> 19) & 3 == 3 and (h >> 17) & 3 == 1, "Expected MPEG1 Layer III"
        rate = rates[(h >> 12) & 15]
        sample_rate = [44100, 48000, 32000][(h >> 10) & 3]
        size = 144000 * rate // sample_rate + ((h >> 9) & 1)
        if offset + size > len(data):
            break
        result.append((data[offset:offset + size], 1152 / sample_rate, rate))
        offset += size
    if complete:
        assert offset == len(data), (offset, len(data))
    return result


def adts_frames(data):
    result, offset = [], 0
    while offset < len(data):
        assert data[offset] == 255 and data[offset + 1] & 240 == 240
        size = ((data[offset + 3] & 3) << 11) | (data[offset + 4] << 3) | (data[offset + 5] >> 5)
        result.append((data[offset:offset + size], 1024 / 48000, 96))
        offset += size
    assert offset == len(data)
    return result


class Upstream:
    def __init__(self, frames, content_type):
        self.frames, self.content_type = frames, content_type
        self.mode = "normal"
        self.connections = 0
        self.stopping = threading.Event()
        owner = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *_):
                pass

            def do_GET(self):
                owner.connections += 1
                if owner.mode == "down":
                    self.send_error(503)
                    return
                self.send_response(200)
                self.send_header("Content-Type", owner.content_type)
                self.end_headers()
                deadline, index = time.monotonic(), 0
                try:
                    while not owner.stopping.is_set():
                        if owner.mode == "down":
                            return
                        if owner.mode == "stall":
                            owner.stopping.wait(0.02)
                            deadline = time.monotonic()
                            continue
                        frame, duration, _ = owner.frames[index % len(owner.frames)]
                        self.wfile.write(frame)
                        self.wfile.flush()
                        deadline += duration
                        owner.stopping.wait(max(0, deadline - time.monotonic()))
                        index += 1
                except (BrokenPipeError, ConnectionResetError):
                    pass

        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.url = f"http://127.0.0.1:{self.server.server_port}/stream"

    def close(self):
        self.stopping.set()
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()


class Listener:
    def __init__(self, port, path, half_close=False):
        self.socket = socket.create_connection(("127.0.0.1", port), timeout=3)
        self.socket.sendall(f"GET {path} HTTP/1.0\r\n\r\n".encode())
        if half_close:
            self.socket.shutdown(socket.SHUT_WR)
        raw = b""
        while b"\r\n\r\n" not in raw:
            raw += self.socket.recv(4096)
        headers, body = raw.split(b"\r\n\r\n", 1)
        assert headers.startswith(b"HTTP/1.0 200"), headers
        assert b"Content-Type: audio/mpeg" in headers
        self.data = bytearray(body)
        self.last = time.monotonic()
        self.max_gap = 0
        self.error = None
        self.done = threading.Event()
        self.thread = threading.Thread(target=self.read, daemon=True)
        self.thread.start()

    def read(self):
        try:
            while not self.done.is_set():
                chunk = self.socket.recv(65536)
                if not chunk:
                    raise AssertionError("Listener disconnected")
                now = time.monotonic()
                self.max_gap = max(self.max_gap, now - self.last)
                self.last = now
                self.data.extend(chunk)
        except (OSError, AssertionError) as exc:
            if not self.done.is_set():
                self.error = exc

    def close(self):
        self.done.set()
        self.socket.shutdown(socket.SHUT_RDWR)
        self.socket.close()
        self.thread.join()


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def audio_kind(data):
    # Captures may begin mid-frame when we inspect their recent tail.
    for offset in range(min(1500, len(data) - 4)):
        if data[offset] == 255 and data[offset + 1] & 0xfe == 0xfa:
            try:
                frames = mp3_frames(data[offset:], complete=False)
            except (AssertionError, IndexError):
                continue
            if frames:
                data = b"".join(f[0] for f in frames)
                break
    else:
        raise AssertionError("Cannot find MP3 frame boundary")
    pcm = run("ffmpeg", "-v", "error", "-f", "mp3", "-i", "pipe:0", "-f", "f32le",
              "-ac", "1", "-ar", "8000", "pipe:1", input=bytes(data))
    samples = array.array("f", pcm)
    if not samples:
        raise AssertionError("No decoded audio")
    # Inspect only the most recent second; startup/transition audio is irrelevant.
    samples = samples[-8000:]
    rms = math.sqrt(sum(x*x for x in samples) / len(samples))
    if rms < 0.001:
        return "silence"
    crossings = sum(a <= 0 < b for a, b in zip(samples, samples[1:]))
    hz = crossings * 8000 / len(samples)
    return "primary" if abs(hz - 440) < 30 else "backup" if abs(hz - 880) < 30 else f"{hz:.1f}Hz"


def http_request(port, request):
    with socket.create_connection(("127.0.0.1", port), timeout=3) as client:
        client.sendall(request)
        response = b""
        while True:
            block = client.recv(4096)
            if not block:
                return response
            response += block


def offline_startup(temp, upstream):
    upstream.mode = "down"
    port = free_port()
    config = temp / "offline.toml"
    config.write_text(f'''listen = "127.0.0.1:{port}"
streams = ["{upstream.url}"]
[[outputs]]
path = "/music/high.mp3"
bitrate = 320
[[outputs]]
path = "/music/low.mp3"
bitrate = 64
''')
    log_path = temp / "offline.log"
    with log_path.open("wb") as log:
        process = subprocess.Popen([str(BIN), str(config)], stdout=log, stderr=log)
        listeners = []
        try:
            wait_for(lambda: "Serving" in log_path.read_text(), message="offline startup")
            # The pipeline must run before any listener arrives.
            time.sleep(1)
            listeners = [Listener(port, "/music/high.mp3"), Listener(port, "/music/low.mp3")]
            time.sleep(1.5)
            for listener in listeners:
                assert listener.error is None, listener.error
                assert audio_kind(listener.data) == "silence"
            cases = [(b"GET /unknown HTTP/1.0\r\n\r\n", 404),
                     (b"POST /music/high.mp3 HTTP/1.0\r\n\r\n", 405),
                     (b"GET /music/high.mp3 INVALID\r\n\r\n", 400),
                     (b"GET / HTTP/1.0\r\nX: " + b"a" * (4095 - len(b"GET / HTTP/1.0\r\nX: ")), 431),
                     (b"HEAD /music/high.mp3 HTTP/1.0\r\n\r\n", 200)]
            for request, status in cases:
                response = http_request(port, request)
                assert response.startswith(f"HTTP/1.0 {status}".encode()), response
                if request.startswith(b"HEAD"):
                    assert response.endswith(b"\r\n\r\n")
            for listener in listeners:
                listener.close()
            listeners.clear()
            process.terminate()
            process.wait(timeout=5)
            assert process.returncode == 0, log_path.read_text()
            print("PASS: offline startup, late joins, custom paths and HTTP errors/HEAD", flush=True)
        finally:
            for listener in listeners:
                listener.close()
            if process.poll() is None:
                process.kill()
                process.wait()
            upstream.mode = "normal"


def main():
    started = time.monotonic()
    with tempfile.TemporaryDirectory(prefix="permastream-test-") as directory:
        temp = Path(directory)
        primary_bytes = run("ffmpeg", "-v", "error", "-f", "lavfi", "-i",
                            "sine=frequency=440:sample_rate=44100", "-t", "3",
                            "-ac", "2", "-c:a", "libmp3lame", "-b:a", "128k",
                            "-write_xing", "0", "-id3v2_version", "0", "-f", "mp3", "pipe:1")
        backup_bytes = run("ffmpeg", "-v", "error", "-f", "lavfi", "-i",
                           "sine=frequency=880:sample_rate=48000", "-t", "3",
                           "-ac", "1", "-c:a", "aac", "-b:a", "96k", "-f", "adts", "pipe:1")
        primary = Upstream(mp3_frames(primary_bytes), "audio/mpeg")
        backup = Upstream(adts_frames(backup_bytes), "audio/aac")
        offline_startup(temp, primary)
        port = free_port()
        config = temp / "test.toml"
        config.write_text(f'''listen = "127.0.0.1:{port}"
streams = ["{primary.url}", "{backup.url}"]
buffer_seconds = 0.4
timeout_seconds = 2
recovery_seconds = 1
retry_seconds = 0.2
retry_max_seconds = 0.5
client_buffer_seconds = 0.2
max_clients = 12
''')
        run(str(BIN), "--check", str(config))
        # Bad config must fail before opening sockets or starting worker threads.
        invalid = ["streams = []", 'streams = ["file:///etc/passwd"]',
                   'streams = ["http://localhost/x"]\nbufer_seconds = 1',
                   'streams = ["http://localhost/x"]\nbuffer_seconds = nan',
                   'streams = ["http://localhost/x"]\nlisten = "broken"',
                   'streams = ["http://localhost/x"]\n[[outputs]]\npath = "/x"\nbitrate = 65',
                   'streams = ["http://localhost/x"]\n[[outputs]]\npath = "/x"\nbitrate = 64\n'
                   '[[outputs]]\npath = "/x"\nbitrate = 320']
        for text in invalid:
            bad = temp / "invalid.toml"
            bad.write_text(text)
            result = subprocess.run([str(BIN), "--check", str(bad)], capture_output=True)
            assert result.returncode == 1, (text, result)
        log_path = temp / "relay.log"
        log_file = log_path.open("wb")
        process = subprocess.Popen([str(BIN), str(config)], stdout=log_file, stderr=log_file)
        listeners, slow = [], None
        try:
            wait_for(lambda: "Serving" in log_path.read_text(), message="HTTP server startup")
            listeners = [Listener(port, "/local.mp3"), Listener(port, "/remote.mp3"),
                         Listener(port, "/local.mp3", half_close=True)]

            def expect(kind, timeout=12):
                def selected():
                    lines = [line for line in log_path.read_text().splitlines() if "Output:" in line]
                    return lines[-1] if lines else ""
                match = f"Output: source {1 if kind == 'primary' else 2}" if kind != "silence" else "Output: silence;"
                wait_for(lambda: match in selected(), timeout, kind)
                time.sleep(2)
                for listener in listeners:
                    assert listener.error is None, listener.error
                    assert audio_kind(listener.data[-100000:]) == kind, (kind, audio_kind(listener.data[-100000:]))

            expect("primary")
            print("PASS: concurrent 320/64 kbps clients, half-closed request, primary audio", flush=True)
            primary.mode = "down"
            expect("backup")
            print("PASS: disconnect failover to 48 kHz mono AAC input", flush=True)
            primary.mode = "normal"
            expect("primary")
            primary.mode = "stall"
            expect("backup")
            print("PASS: preferred-source recovery and open-socket stall failover", flush=True)
            backup.mode = "down"
            expect("silence")
            print("PASS: continuous encoded silence during total outage", flush=True)
            primary.mode = "normal"
            expect("primary")
            # Leave a client unread until both its TCP window and relay buffer fill.
            slow = socket.socket()
            slow.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)
            slow.connect(("127.0.0.1", port))
            slow.sendall(b"GET /local.mp3 HTTP/1.0\r\n\r\n")
            wait_for(lambda: "Dropping slow client" in log_path.read_text(), timeout=12,
                     message="slow-client eviction")
            print("PASS: slow listener evicted without affecting other clients", flush=True)
            for index, listener in enumerate(listeners):
                assert listener.error is None, listener.error
                assert listener.max_gap < 1.0, listener.max_gap
                frames = mp3_frames(bytes(listener.data), complete=False)
                expected_rate = 64 if index == 1 else 320
                assert all(f[2] == expected_rate for f in frames)
                # Reservoir disabled: main_data_begin is zero for every frame.
                assert all(f[0][4] == 0 and f[0][5] & 128 == 0 for f in frames)
                capture = temp / f"capture-{index}.mp3"
                capture.write_bytes(b"".join(f[0] for f in frames))
                probe = json.loads(run("ffprobe", "-v", "error", "-show_streams",
                                       "-of", "json", str(capture)))["streams"][0]
                assert probe["codec_name"] == "mp3" and int(probe["bit_rate"]) == expected_rate * 1000
                run("ffmpeg", "-v", "error", "-xerror", "-i", str(capture), "-f", "null", "-")
            for listener in listeners:
                listener.close()
            listeners.clear()
            process.send_signal(signal.SIGTERM)
            process.wait(timeout=5)
            assert process.returncode == 0, process.returncode
            print(f"PASS: every captured frame decodes; clean shutdown ({time.monotonic() - started:.1f}s)", flush=True)
        except BaseException:
            print(log_path.read_text(), flush=True)
            raise
        finally:
            for listener in listeners:
                listener.close()
            if slow:
                slow.close()
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
            log_file.close()
            primary.close()
            backup.close()


if __name__ == "__main__":
    main()
