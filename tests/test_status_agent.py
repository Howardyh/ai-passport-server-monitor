import http.client
import importlib.util
import json
from pathlib import Path
import secrets
import threading
from types import SimpleNamespace
import unittest
from unittest.mock import patch
from http.server import HTTPServer

spec = importlib.util.spec_from_file_location("passport_status", Path(__file__).resolve().parents[1]/"server/passport_status.py")
agent = importlib.util.module_from_spec(spec)
spec.loader.exec_module(agent)


class AgentTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.token = secrets.token_urlsafe(32)  # ephemeral test fixture, never printed
        cls.snapshot = agent.Snapshot()
        cls.server = HTTPServer(("127.0.0.1", 0), agent.handler_for(cls.token, cls.snapshot))
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown(); cls.server.server_close(); cls.thread.join()

    def request(self, method="GET", path="/api/v1/status", auth=True):
        conn = http.client.HTTPConnection("127.0.0.1", self.server.server_port, timeout=3)
        headers = {"Authorization": "Bearer " + self.token} if auth else {}
        conn.request(method, path, headers=headers)
        response = conn.getresponse()
        result = response.status, dict(response.getheaders()), response.read()
        conn.close()
        return result

    def test_authentication(self):
        status, headers, body = self.request(auth=False)
        self.assertEqual(status, 401); self.assertEqual(headers["Cache-Control"], "no-store")
        self.assertNotIn(self.token.encode(), body)

    def test_route_and_read_only(self):
        self.assertEqual(self.request(path="/wrong")[0], 404)
        self.assertEqual(self.request(path="/api/v1/status?x=1")[0], 404)
        for method in ("POST", "PUT", "DELETE", "PATCH", "HEAD", "OPTIONS"):
            self.assertEqual(self.request(method=method)[0], 405)

    def test_snapshot_and_stale(self):
        data = {"version":1,"cpu":{"temperature":None}}
        self.snapshot.store(data)
        status, headers, body = self.request()
        self.assertEqual(status,200); self.assertEqual(json.loads(body), data)
        self.assertEqual(headers["Content-Type"],"application/json")
        self.assertEqual(headers["Cache-Control"],"no-store")
        self.snapshot.updated -= 11
        self.assertEqual(self.request()[0],503)

    def test_temperature_absent(self):
        with patch.object(agent.psutil, "sensors_temperatures", return_value={}, create=True):
            self.assertIsNone(agent.cpu_temperature())

    def test_bounds_and_nonfinite(self):
        with self.assertRaises(ValueError): self.snapshot.store({"data":"x"*5000})
        with self.assertRaises(ValueError): self.snapshot.store({"value":float("nan")})
        with self.assertRaises(ValueError): agent.active("nginx;id.service")
        with self.assertRaises(ValueError): agent.active("--help.service")

    def test_root_refused(self):
        with patch.object(agent.os, "geteuid", return_value=0, create=True):
            with self.assertRaises(SystemExit): agent.main()

    def test_collector_schema_and_counter_reset(self):
        config = {}
        memory = SimpleNamespace(total=4096000000, available=1996000000, percent=51.3)
        disk = SimpleNamespace(total=100000000000, used=45000000000, percent=45)
        with patch.object(agent, "network_totals", side_effect=[(1000,2000),(500,1000)]), \
             patch.object(agent, "active", return_value=True), \
             patch.object(agent, "cpu_temperature", return_value=None), \
             patch.object(agent.psutil, "cpu_percent", return_value=23.4), \
             patch.object(agent.psutil, "virtual_memory", return_value=memory), \
             patch.object(agent.psutil, "disk_usage", return_value=disk), \
             patch.object(agent.psutil, "boot_time", return_value=1), \
             patch.object(agent.os, "getloadavg", return_value=(0.53,0.38,0.29), create=True):
            result = agent.Collector(config).sample()
        self.assertEqual(set(result), {"version","timestamp","hostname","uptime","cpu","memory","disk","network","services"})
        self.assertIsNone(result["cpu"]["temperature"])
        self.assertEqual(result["network"]["rx_bps"], 0)
        self.assertEqual(result["network"]["tx_bps"], 0)
        self.assertEqual(result["memory"]["used"], 2100000000)


if __name__ == "__main__": unittest.main()
