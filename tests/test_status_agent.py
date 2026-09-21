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


class AgentHTTPTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        from aiohttp.test_utils import TestClient, TestServer
        self.token = secrets.token_urlsafe(32)
        self.snapshot = agent.Snapshot()
        self.snapshot.store({"v":1,"type":"status","seq":1,"timestamp":1790000000,"cpu":{"usage":10,"temperature":None},"memory":{"percent":20},"disk":{"percent":30}})
        self.client = TestClient(TestServer(agent.make_app(self.token, self.snapshot, push_interval=0.02)))
        await self.client.start_server()
        self.headers = {"Authorization": "Bearer " + self.token}

    async def asyncTearDown(self):
        await self.client.close()

    async def test_authentication(self):
        for path in ("/ws", "/api/v1/status"):
            response = await self.client.get(path)
            self.assertEqual(response.status, 401)
            self.assertEqual(response.headers["Cache-Control"], "no-store")
            response = await self.client.get(path, headers=[("Authorization",self.headers["Authorization"])]*2)
            self.assertEqual(response.status, 401)

    async def test_routes(self):
        for path in ("/wrong", "/ws?token=bad", "/api/v1/status?x=1"):
            self.assertEqual((await self.client.get(path, headers=self.headers)).status, 404)
        for method in ("POST", "PUT", "DELETE", "PATCH", "HEAD", "OPTIONS"):
            self.assertEqual((await self.client.request(method,"/api/v1/status",headers=self.headers)).status,405)

    async def test_snapshot_stale_and_websocket(self):
        response = await self.client.get("/api/v1/status",headers=self.headers)
        self.assertEqual(response.status,200)
        self.assertEqual(response.headers["Cache-Control"],"no-store")
        expected = await response.json()
        ws = await self.client.ws_connect("/ws",headers=self.headers)
        self.assertEqual(await ws.receive_json(timeout=2),expected)
        await ws.ping()
        self.snapshot.updated -= 11
        self.assertEqual((await self.client.get("/api/v1/status",headers=self.headers)).status,503)
        await ws.close()

    async def test_broadcast_shared_and_alert_edge(self):
        first = await self.client.ws_connect("/ws",headers=self.headers)
        second = await self.client.ws_connect("/ws",headers=self.headers)
        await first.receive_json(timeout=2)
        await second.receive_json(timeout=2)
        data = json.loads(self.snapshot.read());data["seq"]=2;data["cpu"]["usage"]=95
        self.snapshot.store(data)
        async def next_type(ws,kind):
            for _ in range(8):
                value=await ws.receive_json(timeout=2)
                if value["type"]==kind and (kind!="status" or value["seq"]==2): return value
            self.fail("Expected message absent")
        self.assertEqual(await next_type(first,"status"),await next_type(second,"status"))
        alert=await next_type(first,"alert")
        self.assertEqual(alert["source"],"cpu");self.assertEqual(alert,await next_type(second,"alert"))
        await first.close();await second.close()


class AgentLogicTests(unittest.TestCase):
    def test_alert_edges(self):
        engine=agent.AlertEngine()
        data={"seq":1,"timestamp":1,"cpu":{"usage":91},"memory":{"percent":20},"disk":{"percent":20}}
        self.assertEqual(len(engine.edges(data)),1)
        self.assertEqual(engine.edges(data),[])
        data["cpu"]["usage"]=89;self.assertEqual(engine.edges(data),[])
        data["cpu"]["usage"]=95;self.assertEqual(len(engine.edges(data)),1)

    def test_temperature_absent(self):
        with patch.object(agent.psutil, "sensors_temperatures", return_value={}, create=True):
            self.assertIsNone(agent.cpu_temperature())

    def test_bounds_and_nonfinite(self):
        with self.assertRaises(ValueError): agent.Snapshot().store({"data":"x"*5000})
        with self.assertRaises(ValueError): agent.Snapshot().store({"value":float("nan")})
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
        self.assertEqual(set(result), {"v","type","seq","timestamp","hostname","uptime","cpu","memory","disk","network","services"})
        self.assertIsNone(result["cpu"]["temperature"])
        self.assertEqual(result["network"]["rx_bps"], 0)
        self.assertEqual(result["network"]["tx_bps"], 0)
        self.assertEqual(result["memory"]["used"], 2100000000)


if __name__ == "__main__": unittest.main()
