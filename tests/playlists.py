#!/usr/bin/env python3
"""Exercise real remote playlists, local PLS, fallback, redirects and URL queries."""
import http.server
import json
from pathlib import Path
import subprocess
import tempfile
import threading
import time

from integration import BIN, Listener, Upstream, adts_frames, audio_kind, free_port, mp3_frames, run, wait_for


def main():
    with tempfile.TemporaryDirectory(prefix="permastream-playlists-") as directory:
        temp = Path(directory)
        tone = run("ffmpeg", "-v", "error", "-f", "lavfi", "-i",
                   "sine=frequency=440:sample_rate=44100", "-t", "3", "-ac", "2",
                   "-c:a", "libmp3lame", "-b:a", "128k", "-write_xing", "0",
                   "-id3v2_version", "0", "-f", "mp3", "pipe:1")
        upstream = Upstream(mp3_frames(tone), "audio/mpeg")
        backup_tone = run("ffmpeg", "-v", "error", "-f", "lavfi", "-i",
                          "sine=frequency=880:sample_rate=48000", "-t", "3", "-ac", "1",
                          "-c:a", "aac", "-b:a", "96k", "-f", "adts", "pipe:1")
        backup = Upstream(adts_frames(backup_tone), "audio/aac")
        requests = []
        query = "?key=fixture%2Bkey&second=keep%2Fthis"
        refresh = False

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *_):
                pass

            def do_GET(self):
                requests.append(self.path)
                if self.path == "/start":
                    self.send_response(302)
                    self.send_header("Location", "/dir/list.m3u?playlist=fixture")
                    self.end_headers()
                    return
                if self.path == "/dir/list.m3u?playlist=fixture":
                    # Use the final redirect URL as the base. Preserve query bytes.
                    body = "\ufeff#EXTM3U\r\n#EXTINF:-1,Example\r\n../bad\r\nsub/../audio" + query + "\r\n"
                    content_type = "audio/x-mpegurl"
                elif self.path in ("/dir/audio" + query, "/audio" + query):
                    self.send_response(302)
                    self.send_header("Location", upstream.url)
                    self.end_headers()
                    return
                elif self.path == "/radio.pls":
                    # Numeric order must win over physical line order.
                    body = "[playlist]\nFile3=/audio" + query + "\nTitle3=Example\nFile1=/bad\nFile2=file:///must-not-open.mp3\nNumberOfEntries=3\nVersion=2\n"
                    content_type = "audio/x-scpls"
                elif self.path == "/failover.pls":
                    body = "[playlist]\nFile1=/audio" + query + "\nFile2=/backup\n"
                    content_type = "audio/x-scpls"
                elif self.path == "/backup":
                    self.send_response(302)
                    self.send_header("Location", backup.url)
                    self.end_headers()
                    return
                elif self.path == "/plain":
                    body = f"http://127.0.0.1:{self.server.server_port}/audio{query}\n"
                    content_type = "text/plain"
                elif self.path == "/nested.pls":
                    body = "[playlist]\nFile1=/cycle.m3u\nFile2=/radio.pls\n"
                    content_type = "text/plain"
                elif self.path == "/cycle.m3u":
                    body = "#EXTM3U\n/cycle.m3u\n"
                    content_type = "audio/x-mpegurl"
                elif self.path == "/large.pls":
                    body = "[playlist]\n" + "#" * (256 * 1024)
                    content_type = "audio/x-scpls"
                elif self.path == "/refresh.pls":
                    body = "[playlist]\nFile1=" + ("/audio" + query if refresh else "/bad") + "\n"
                    content_type = "audio/x-scpls"
                else:
                    self.send_error(503)
                    return
                data = body.encode()
                self.send_response(200)
                self.send_header("Content-Type", content_type)
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                try:
                    self.wfile.write(data)
                except (BrokenPipeError, ConnectionResetError):
                    pass

        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        origin = f"http://127.0.0.1:{server.server_port}"
        (temp / "private.pls").write_text(f"[playlist]\nFile1={origin}/bad\nFile2={origin}/radio.pls\n")
        (temp / "large.pls").write_text("[playlist]\n" + "#" * (256 * 1024))
        try:
            cases = [([origin + "/start"], "redirected remote M3U"),
                     ([origin + "/radio.pls"], "ordered remote PLS"),
                     ([origin + "/plain"], "extensionless plain M3U"),
                     ([origin + "/nested.pls"], "nested playlists and cycle rejection"),
                     (["private.pls"], "local PLS relative to config directory"),
                     ([origin + "/failover.pls"], "active playlist entry failover"),
                     ([origin + "/large.pls", "large.pls", origin + "/radio.pls"], "oversized playlist isolation"),
                     ([origin + "/refresh.pls"], "playlist refresh on retry")]
            for sources, label in cases:
                requests.clear()
                port = free_port()
                config = temp / "config.toml"
                config.write_text(f'''listen = "127.0.0.1:{port}"
streams = {json.dumps(sources)}
buffer_seconds = 0.4
timeout_seconds = 2
retry_seconds = 0.2
retry_max_seconds = 0.5
''')
                log_path = temp / "relay.log"
                with log_path.open("wb") as log:
                    process = subprocess.Popen([str(BIN), str(config)], stdout=log, stderr=log)
                    listeners = []
                    try:
                        wait_for(lambda: "Serving" in log_path.read_text(), message=label + " startup")
                        listeners = [Listener(port, "/local.mp3"), Listener(port, "/remote.mp3")]
                        if label == "playlist refresh on retry":
                            wait_for(lambda: requests.count('/refresh.pls') >= 2, message="playlist retries")
                            refresh = True
                        wait_for(lambda: "Output: source" in log_path.read_text(), message=label + " audio")
                        time.sleep(1.6)
                        for listener in listeners:
                            assert listener.error is None, listener.error
                            assert audio_kind(listener.data[-100000:]) == "primary", log_path.read_text()
                            assert listener.max_gap < 1.0, listener.max_gap
                        if label == "active playlist entry failover":
                            upstream.mode = "down"
                            wait_for(lambda: '/backup' in requests, message="next playlist entry")
                            time.sleep(3)
                            for listener in listeners:
                                assert listener.error is None, listener.error
                                assert audio_kind(listener.data[-100000:]) == "backup", log_path.read_text()
                                assert listener.max_gap < 1.0, listener.max_gap
                        assert any(path.endswith(query) for path in requests), requests
                        if label == "ordered remote PLS":
                            assert requests.index('/bad') < requests.index('/audio' + query), requests
                        if label == "nested playlists and cycle rejection":
                            assert requests.count('/cycle.m3u') == 1, requests
                        assert "fixture%2Bkey" not in log_path.read_text()
                        assert origin not in log_path.read_text()
                        for listener in listeners:
                            listener.close()
                        listeners.clear()
                        process.terminate()
                        process.wait(timeout=5)
                        assert process.returncode == 0, log_path.read_text()
                        print("PASS:", label, flush=True)
                    except BaseException:
                        print(log_path.read_text(), flush=True)
                        raise
                    finally:
                        for listener in listeners:
                            listener.close()
                        if process.poll() is None:
                            process.terminate()
                            try:
                                process.wait(timeout=5)
                            except subprocess.TimeoutExpired:
                                process.kill()
                                process.wait()
                        upstream.mode = "normal"
        finally:
            upstream.close()
            backup.close()
            server.shutdown()
            server.server_close()
            thread.join()


if __name__ == "__main__":
    main()
