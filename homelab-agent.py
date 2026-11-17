#!/usr/bin/env python3

"""
HomeLab Monitoring Agent

HTTP endpoints:
    GET /health   -> {"status": "online"}
    GET /metrics  -> JSON system and Docker metrics

Default listener:
    0.0.0.0:8765
"""

import json
import os
import shutil
import subprocess
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


HOST = "0.0.0.0"
PORT = 8765
DISK_PATH = "/"


def cpu_read():
    """Read aggregate CPU counters from /proc/stat."""
    with open("/proc/stat", "r", encoding="utf-8") as file:
        values = list(map(int, file.readline().split()[1:]))

    idle = values[3] + (values[4] if len(values) > 4 else 0)
    total = sum(values)
    return idle, total


def cpu_usage():
    """Calculate CPU utilisation over a short sampling interval."""
    idle_before, total_before = cpu_read()
    time.sleep(0.15)
    idle_after, total_after = cpu_read()

    total_delta = total_after - total_before
    idle_delta = idle_after - idle_before

    if total_delta <= 0:
        return 0.0

    usage = 100.0 * (1.0 - (idle_delta / total_delta))
    return round(max(0.0, min(100.0, usage)), 1)


def memory_read():
    """Read RAM statistics from /proc/meminfo."""
    values = {}

    with open("/proc/meminfo", "r", encoding="utf-8") as file:
        for line in file:
            parts = line.split()
            if len(parts) >= 2:
                values[parts[0].rstrip(":")] = int(parts[1]) * 1024

    total = values.get("MemTotal", 0)
    available = values.get("MemAvailable", values.get("MemFree", 0))
    used = max(0, total - available)

    return {
        "total": total,
        "used": used,
        "available": available,
        "percent": round((used / total) * 100, 1) if total else 0.0,
    }


def disk_read():
    """Read disk usage for the configured filesystem."""
    usage = shutil.disk_usage(DISK_PATH)

    return {
        "total": usage.total,
        "used": usage.used,
        "free": usage.free,
        "percent": round((usage.used / usage.total) * 100, 1)
        if usage.total else 0.0,
    }


def uptime_read():
    """Return system uptime in seconds."""
    with open("/proc/uptime", "r", encoding="utf-8") as file:
        return round(float(file.readline().split()[0]), 1)


def docker_read():
    """Return Docker availability and container details."""
    docker_path = shutil.which("docker")

    result = {
        "available": False,
        "total": 0,
        "running": 0,
        "stopped": 0,
        "containers": [],
    }

    if not docker_path:
        return result

    try:
        check = subprocess.run(
            [docker_path, "info", "--format", "{{.ServerVersion}}"],
            capture_output=True,
            text=True,
            timeout=3,
            check=False,
        )

        if check.returncode != 0:
            return result

        result["available"] = True
        result["version"] = check.stdout.strip()

        containers = subprocess.run(
            [
                docker_path,
                "ps",
                "-a",
                "--format",
                "{{json .}}",
            ],
            capture_output=True,
            text=True,
            timeout=5,
            check=False,
        )

        if containers.returncode != 0:
            return result

        for line in containers.stdout.splitlines():
            line = line.strip()
            if not line:
                continue

            try:
                item = json.loads(line)
            except json.JSONDecodeError:
                continue

            state = item.get("State", "unknown")
            container = {
                "id": item.get("ID", ""),
                "name": item.get("Names", ""),
                "image": item.get("Image", ""),
                "state": state,
                "status": item.get("Status", ""),
            }

            result["containers"].append(container)

            if state.lower() == "running":
                result["running"] += 1

        result["total"] = len(result["containers"])
        result["stopped"] = result["total"] - result["running"]

    except (OSError, subprocess.TimeoutExpired):
        return result

    return result


def collect_metrics():
    """Collect all metrics and return one JSON-compatible dictionary."""
    try:
        hostname = os.uname().nodename
    except AttributeError:
        hostname = "unknown"

    return {
        "hostname": hostname,
        "timestamp": int(time.time()),
        "cpu": {
            "usage": cpu_usage(),
            "cores": os.cpu_count() or 1,
        },
        "memory": memory_read(),
        "disk": disk_read(),
        "uptime": uptime_read(),
        "docker": docker_read(),
    }


class MonitoringHandler(BaseHTTPRequestHandler):
    """HTTP request handler for the monitoring API."""

    def send_json(self, data, status=200):
        payload = json.dumps(data, separators=(",", ":")).encode("utf-8")

        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(payload)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(payload)

    def do_GET(self):
        if self.path == "/health":
            self.send_json({"status": "online"})
            return

        if self.path == "/metrics":
            try:
                self.send_json(collect_metrics())
            except Exception as error:
                self.send_json(
                    {"error": "Unable to collect metrics", "detail": str(error)},
                    status=500,
                )
            return

        self.send_json({"error": "Not found"}, status=404)

    def log_message(self, format_string, *args):
        """Keep routine HTTP requests out of the system journal."""
        return


def main():
    server = ThreadingHTTPServer((HOST, PORT), MonitoringHandler)
    server.daemon_threads = True

    print(f"HomeLab Monitoring Agent listening on {HOST}:{PORT}", flush=True)

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("Stopping HomeLab Monitoring Agent", flush=True)
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
