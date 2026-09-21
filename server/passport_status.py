#!/usr/bin/env python3
"""Read-only, loopback-only Linux status service. No shell, SSH or root."""
from __future__ import annotations

import hmac
import json
import logging
import math
import os
from pathlib import Path
import re
import socket
import subprocess
import threading
import time
import asyncio
from aiohttp import web

import psutil

LOG = logging.getLogger("passport-status")
ENV_FILE = Path("/etc/passport-status.env")
MAX_SNAPSHOT_AGE = 10.0
TOKEN_RE = re.compile(r"[A-Za-z0-9._~+/=-]{32,128}\Z")
UNIT_RE = re.compile(r"[A-Za-z0-9_.@-]+\.service\Z")


def settings() -> dict[str, str]:
    # systemd reads EnvironmentFile as root before running this process as User=.
    values = {key: value for key, value in os.environ.items() if key.startswith("PASSPORT_STATUS_")}
    if "PASSPORT_STATUS_TOKEN" not in values:
        for line in ENV_FILE.read_text(encoding="utf-8").splitlines():
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            key, sep, value = line.partition("=")
            if not sep or not key.startswith("PASSPORT_STATUS_"):
                raise ValueError("Invalid settings file")
            values[key] = value.strip().strip('"').strip("'")
    if not TOKEN_RE.fullmatch(values.get("PASSPORT_STATUS_TOKEN", "")):
        raise ValueError("Configure a 32-128 character random token")
    return values


def active(unit: str) -> bool:
    if not UNIT_RE.fullmatch(unit) or unit.startswith("-"):
        raise ValueError("Invalid systemd unit name")
    try:
        result = subprocess.run(
            ["/usr/bin/systemctl", "is-active", "--quiet", unit],
            stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL, timeout=0.75, check=False,
        )
        return result.returncode == 0
    except (OSError, subprocess.TimeoutExpired):
        return False


def cpu_temperature() -> float | None:
    try:
        groups = psutil.sensors_temperatures()
        # Never substitute a disk/NVMe sensor for CPU temperature.
        for group in ("coretemp", "k10temp", "cpu_thermal", "zenpower"):
            for sensor in groups.get(group, []):
                value = sensor.current
                if value is not None and math.isfinite(value) and -100 <= value <= 250:
                    return round(float(value), 1)
    except (AttributeError, OSError, NotImplementedError):
        pass
    return None


def network_totals(interface: str) -> tuple[int, int]:
    counters = psutil.net_io_counters(pernic=True)
    if interface:
        if interface not in counters:
            raise ValueError("Configured network interface is unavailable")
        selected = [counters[interface]]
    else:
        selected = [value for name, value in counters.items() if name != "lo"]
    return sum(v.bytes_recv for v in selected), sum(v.bytes_sent for v in selected)


class Collector:
    def __init__(self, config: dict[str, str]):
        self.disk_path = config.get("PASSPORT_STATUS_DISK", "/")
        self.interface = config.get("PASSPORT_STATUS_INTERFACE", "")
        self.units = {
            "nginx": config.get("PASSPORT_STATUS_NGINX_UNIT", "nginx.service"),
            "mariadb": config.get("PASSPORT_STATUS_MARIADB_UNIT", "mariadb.service"),
            "php_fpm": config.get("PASSPORT_STATUS_PHP_FPM_UNIT", "php8.3-fpm.service"),
        }
        if not all(UNIT_RE.fullmatch(unit) and not unit.startswith("-") for unit in self.units.values()):
            raise ValueError("Invalid unit name")
        self.previous = network_totals(self.interface)
        self.previous_at = time.monotonic()
        self.services = {name: False for name in self.units}
        self.next_services = 0.0
        self.seq = int(time.time()*1000)
        psutil.cpu_percent(interval=None)  # prime measurement on the sampling thread

    def sample(self) -> dict:
        if time.monotonic() >= self.next_services:
            self.services = {name: active(unit) for name, unit in self.units.items()}
            self.next_services = time.monotonic() + 5
        now = time.monotonic()
        rx, tx = network_totals(self.interface)
        elapsed = max(now - self.previous_at, 0.001)
        rx_rate = max(0, rx - self.previous[0]) / elapsed
        tx_rate = max(0, tx - self.previous[1]) / elapsed
        self.previous, self.previous_at = (rx, tx), now
        memory = psutil.virtual_memory()
        disk = psutil.disk_usage(self.disk_path)
        load1, load5, load15 = os.getloadavg()
        hostname = socket.gethostname()
        hostname = "".join(ch if 32 <= ord(ch) < 127 else "?" for ch in hostname)[:63] or "server"
        # RAM used matches the utilization percentage (total minus available).
        self.seq += 1
        return {
            "v": 1, "type": "status", "seq": self.seq, "timestamp": int(time.time()), "hostname": hostname,
            "uptime": max(0, int(time.time() - psutil.boot_time())),
            "cpu": {"usage": psutil.cpu_percent(interval=None), "load1": load1, "load5": load5,
                    "load15": load15, "temperature": cpu_temperature()},
            "memory": {"total": memory.total, "used": memory.total - memory.available, "percent": memory.percent},
            "disk": {"total": disk.total, "used": disk.used, "percent": disk.percent},
            "network": {"rx_bytes": rx, "tx_bytes": tx, "rx_bps": round(rx_rate, 2), "tx_bps": round(tx_rate, 2)},
            "services": dict(self.services),
        }


class Snapshot:
    def __init__(self):
        self.lock = threading.Lock()
        self.body: bytes | None = None
        self.updated = 0.0

    def store(self, data: dict) -> None:
        body = json.dumps(data, separators=(",", ":"), allow_nan=False).encode("utf-8")
        if len(body) > 4096:
            raise ValueError("Snapshot exceeds firmware response limit")
        with self.lock:
            self.body, self.updated = body, time.monotonic()

    def read(self) -> bytes | None:
        with self.lock:
            return self.body if self.body is not None and time.monotonic() - self.updated <= MAX_SNAPSHOT_AGE else None


def collect(config: dict[str, str], snapshot: Snapshot, stop: threading.Event) -> None:
    collector = None
    while not stop.is_set():
        try:
            if collector is None:
                collector = Collector(config)
                if stop.wait(1):
                    break
            snapshot.store(collector.sample())
        except Exception as exc:
            # Log the exception type only: environment values never enter logs.
            LOG.warning("Sampling failed (%s)", type(exc).__name__)
        stop.wait(1)


class AlertEngine:
    def __init__(self):
        self.active = set()
        self.seq = int(time.time()*1000)

    def edges(self, data):
        values = {"cpu": data["cpu"]["usage"], "ram": data["memory"]["percent"], "disk": data["disk"]["percent"]}
        next_active = {name for name, value in values.items() if value >= 90}
        alerts = []
        for name in sorted(next_active - self.active):
            self.seq = max(self.seq+1, data["seq"]+1)
            alerts.append({"v": 1, "type": "alert", "seq": self.seq, "timestamp": data["timestamp"],
                           "level": "critical", "source": name, "value": values[name], "message": name.upper()+" HIGH"})
        self.active = next_active
        return alerts


def make_app(token: str, snapshot: Snapshot, *, push_interval=2.0):
    if not TOKEN_RE.fullmatch(token):
        raise ValueError("Invalid bearer configuration")
    expected = ("Bearer " + token).encode("ascii")
    clients = set()
    reservations = 0
    engine = AlertEngine()

    @web.middleware
    async def authentication(request, handler):
        headers = request.headers.getall("Authorization", [])
        supplied = headers[0].encode("utf-8") if len(headers) == 1 else b""
        if len(supplied) > 256 or not hmac.compare_digest(supplied, expected):
            return web.json_response({"error": "unauthorized"}, status=401, headers={"Cache-Control": "no-store", "WWW-Authenticate": "Bearer"})
        if request.query_string:
            return web.json_response({"error": "query_not_supported"}, status=404)
        return await handler(request)

    async def status(request):
        body = snapshot.read()
        if body is None:
            return web.json_response({"error": "status_unavailable"}, status=503, headers={"Cache-Control": "no-store"})
        return web.Response(body=body, content_type="application/json", headers={"Cache-Control": "no-store"})

    async def websocket(request):
        nonlocal reservations
        if reservations >= 16:
            return web.json_response({"error": "client_limit"}, status=503)
        ws = web.WebSocketResponse(heartbeat=20, receive_timeout=35, max_msg_size=4096, compress=False)
        reservations += 1
        try:
            await ws.prepare(request)
            clients.add(ws)
            body = snapshot.read()
            if body is not None:
                await asyncio.wait_for(ws.send_str(body.decode("utf-8")), timeout=3)
            async for msg in ws:
                if msg.type in (web.WSMsgType.TEXT, web.WSMsgType.BINARY):
                    await ws.close(code=1008, message=b"read-only protocol")
        except (TimeoutError, ConnectionError, RuntimeError):
            pass
        finally:
            clients.discard(ws)
            reservations -= 1
        return ws

    async def send(ws, messages):
        try:
            for message in messages:
                await asyncio.wait_for(ws.send_str(message), timeout=3)
        except (TimeoutError, ConnectionError, RuntimeError):
            clients.discard(ws)
            try:
                await asyncio.wait_for(ws.close(), timeout=1)
            except (TimeoutError, ConnectionError, RuntimeError):
                pass

    async def broadcast():
        last_seq = None
        while True:
            body = snapshot.read()
            if body is not None:
                data = json.loads(body)
                if data["seq"] != last_seq:
                    last_seq = data["seq"]
                    messages = [body.decode("utf-8")]
                    messages += [json.dumps(a, separators=(",", ":"), allow_nan=False) for a in engine.edges(data)]
                    # Sampling is shared; clients only receive the cached serialized snapshot.
                    await asyncio.gather(*(send(ws, messages) for ws in tuple(clients)))
            await asyncio.sleep(push_interval)

    async def lifecycle(app):
        task = asyncio.create_task(broadcast())
        yield
        task.cancel()
        try:
            await task
        except asyncio.CancelledError:
            pass
        await asyncio.gather(*(ws.close(code=1001, message=b"shutdown") for ws in tuple(clients)))

    app = web.Application(middlewares=[authentication], client_max_size=1024)
    app.router.add_get("/api/v1/status", status, allow_head=False)
    app.router.add_get("/ws", websocket, allow_head=False)
    app.cleanup_ctx.append(lifecycle)
    return app


def main() -> None:
    logging.basicConfig(level=logging.INFO, format="%(levelname)s %(message)s")
    if not hasattr(os, "geteuid") or os.geteuid() == 0:
        raise SystemExit("Run on Linux as a dedicated unprivileged user; root is prohibited")
    try:
        config = settings()
    except (OSError, ValueError):
        raise SystemExit("Cannot load valid settings from /etc/passport-status.env") from None
    snapshot, stop = Snapshot(), threading.Event()
    thread = threading.Thread(target=collect, args=(config, snapshot, stop), daemon=True, name="sampler")
    thread.start()
    try:
        web.run_app(make_app(config["PASSPORT_STATUS_TOKEN"], snapshot), host="127.0.0.1", port=8765,
                    access_log=None, print=None)
    finally:
        stop.set()
        thread.join(timeout=5)


if __name__ == "__main__":
    main()
