import http.client
import json
import os
from pathlib import Path
import socket
import ssl
import subprocess
import sys
import tempfile


def serve_response(peer):
    with peer.makefile("rb") as request:
        if request.readline(1024) != b"POST /responses HTTP/1.1\r\n":
            raise RuntimeError("Unexpected HTTPS fixture request line")
        headers = http.client.parse_headers(request)
        for name, expected in (
            ("Accept", "text/event-stream"),
            ("Authorization", "Bearer fixture"),
            ("Content-Type", "application/json"),
            ("Content-Length", "2"),
        ):
            if headers[name] != expected:
                raise RuntimeError(f"Unexpected HTTPS fixture header {name}")
        if request.read(2) != b"{}":
            raise RuntimeError("Unexpected HTTPS fixture request body")

    completion = json.dumps(
        {
            "type": "response.completed",
            "response": {
                "output": [{"text": "終"}],
                "usage": {"input_tokens": 6, "output_tokens": 2, "total_tokens": 8},
            },
        },
        ensure_ascii=False,
    ).encode()
    peer.sendall(
        b"HTTP/1.1 200 OK\r\n"
        b"Content-Type: Text/Event-Stream; charset=utf-8\r\n"
        b"X-Request-Id: fixture-request\r\n"
        b"Transfer-Encoding: chunked\r\n\r\n"
    )
    for chunk in (b": heartbeat\n\n", b"data: " + completion + b"\n", b"\n"):
        peer.sendall(f"{len(chunk):x}\r\n".encode() + chunk + b"\r\n")


def main(executable, openssl):
    with tempfile.TemporaryDirectory(prefix="flock-chatgpt-tls-") as directory:
        certificate = Path(directory) / "loopback.crt"
        key = Path(directory) / "loopback.key"
        generated = subprocess.run(
            [
                openssl, "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                "-keyout", key, "-out", certificate, "-days", "1",
                "-subj", "/CN=127.0.0.1", "-addext", "subjectAltName=IP:127.0.0.1",
            ],
            capture_output=True,
            text=True,
            timeout=10,
        )
        if generated.returncode:
            raise RuntimeError(f"Could not make the loopback TLS certificate: {generated.stderr.strip()}")
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(certificate, key)

        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
            listener.bind(("127.0.0.1", 0))
            listener.listen(1)
            listener.settimeout(5)
            port = listener.getsockname()[1]
            process = subprocess.Popen(
                [executable, "--tls", f"https://127.0.0.1:{port}/responses", certificate],
                env={**os.environ, "NO_PROXY": "127.0.0.1", "no_proxy": "127.0.0.1"},
            )
            try:
                connection, _ = listener.accept()
                with connection:
                    connection.settimeout(5)
                    with context.wrap_socket(connection, server_side=True) as peer:
                        serve_response(peer)
                        # Keep the response unfinished so the client has to stop on completion.
                        if process.wait(timeout=5) != 0:
                            raise RuntimeError("Native ChatGPT SSE request failed")
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait()


if __name__ == "__main__":
    main(*sys.argv[1:])
