#!/usr/bin/env python3
"""Live HLS playback and rejection of an untrusted HTTPS segment server."""
import http.server
from pathlib import Path
import ssl
import subprocess
import tempfile
import threading
import time

from integration import BIN, Listener, audio_kind, free_port, run, wait_for


def main():
    with tempfile.TemporaryDirectory(prefix="permastream-hls-") as directory:
        temp = Path(directory)
        run("ffmpeg", "-v", "error", "-f", "lavfi", "-i",
            "sine=frequency=440:sample_rate=48000", "-t", "30", "-c:a", "aac",
            "-f", "hls", "-hls_time", "1", "-hls_list_size", "0",
            "-hls_segment_filename", str(temp / "seg%03d.ts"), str(temp / "generated.m3u8"))
        run("openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
            "-subj", "/CN=localhost", "-keyout", str(temp / "key.pem"),
            "-out", str(temp / "cert.pem"), "-addext", "subjectAltName=DNS:localhost")
        segment_requests = []
        start = time.monotonic()

        class Segments(http.server.BaseHTTPRequestHandler):
            def log_message(self, *_):
                pass

            def do_GET(self):
                segment_requests.append(self.path)
                path = temp / Path(self.path).name
                if not path.name.startswith("seg") or not path.is_file():
                    self.send_error(404)
                    return
                data = path.read_bytes()
                self.send_response(200)
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

        secure = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Segments)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(str(temp / "cert.pem"), str(temp / "key.pem"))
        secure.socket = context.wrap_socket(secure.socket, server_side=True)
        secure_thread = threading.Thread(target=secure.serve_forever, daemon=True)
        secure_thread.start()

        class Live(Segments):
            def do_GET(self):
                if not self.path.endswith(".m3u8"):
                    return super().do_GET()
                last = min(28, int(time.monotonic() - start) + 3)
                first = max(0, last - 3)
                prefix = f"https://localhost:{secure.server_port}/" if self.path == "/untrusted.m3u8" else ""
                body = "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:2\n"
                body += f"#EXT-X-MEDIA-SEQUENCE:{first}\n"
                for index in range(first, last):
                    body += f"#EXTINF:1.0,\n{prefix}seg{index:03d}.ts\n"
                data = body.encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/vnd.apple.mpegurl")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

        live = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Live)
        thread = threading.Thread(target=live.serve_forever, daemon=True)
        thread.start()
        try:
            for manifest, expected in [("live.m3u8", "primary"), ("untrusted.m3u8", "silence")]:
                port = free_port()
                config = temp / "config.toml"
                config.write_text(f'''listen = "127.0.0.1:{port}"
streams = ["http://127.0.0.1:{live.server_port}/{manifest}"]
buffer_seconds = 2
timeout_seconds = 4
retry_seconds = 0.2
retry_max_seconds = 0.5
''')
                log_path = temp / "relay.log"
                with log_path.open("wb") as log:
                    process = subprocess.Popen([str(BIN), str(config)], stdout=log, stderr=log)
                    listener = None
                    try:
                        wait_for(lambda: "Serving" in log_path.read_text(), message="HLS startup")
                        listener = Listener(port, "/local.mp3")
                        if expected == "primary":
                            wait_for(lambda: "Output: source 1" in log_path.read_text(), message="HLS audio")
                        time.sleep(4)
                        assert listener.error is None, listener.error
                        assert audio_kind(listener.data[-100000:]) == expected, log_path.read_text()
                        if expected == "silence":
                            assert "Output: source 1" not in log_path.read_text(), log_path.read_text()
                        else:
                            assert "Output: silence;" not in log_path.read_text(), log_path.read_text()
                        listener.close()
                        listener = None
                        process.terminate()
                        process.wait(timeout=5)
                        assert process.returncode == 0, log_path.read_text()
                    finally:
                        if listener:
                            listener.close()
                        if process.poll() is None:
                            process.kill()
                            process.wait()
            print("PASS: live HLS audio and certificate validation on nested HTTPS segments", flush=True)
        finally:
            live.shutdown()
            live.server_close()
            secure.shutdown()
            secure.server_close()
            thread.join()
            secure_thread.join()


if __name__ == "__main__":
    main()
