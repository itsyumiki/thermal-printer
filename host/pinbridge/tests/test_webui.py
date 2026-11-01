import unittest

from aiohttp.test_utils import TestClient, TestServer

from pinbridge.webui import WebBackend, create_app


class WebUITests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.backend = WebBackend(mock=True)
        self.client = TestClient(TestServer(create_app(self.backend)))
        await self.client.start_server()
        self.ws = await self.client.ws_connect("/ws")
        hello = await self.ws.receive_json()
        self.assertEqual(hello["type"], "hello")
        self.assertEqual(hello["cfg_hash"], "dfbf0b5f")

    async def asyncTearDown(self):
        await self.ws.close()
        await self.client.close()

    async def send(self, payload):
        await self.ws.send_json(payload)
        return await self.ws.receive_json()

    async def test_serves_assets_and_pin_inventory(self):
        page = await self.client.get("/")
        self.assertEqual(page.status, 200)
        self.assertIn("Hardware console", await page.text())
        script = await self.client.get("/assets/app.js")
        self.assertEqual(script.status, 200)
        info = await (await self.client.get("/api/info")).json()
        names = {pin["name"] for pin in info["pins"]}
        self.assertTrue({"PA0", "PA1", "PC7", "PB5", "DP", "DM"} <= names)

    async def test_mock_connect_and_control(self):
        connected = await self.send({"action": "connect"})
        self.assertEqual(connected["type"], "connected")
        mode = await self.send({"action": "mode", "pin": "PC7", "mode": "hi"})
        self.assertEqual(mode["type"], "result")
        self.assertEqual(
            {p["name"]: p["mode"] for p in mode["status"]["pins"]}["PC7"],
            "hi",
        )
        value = await self.send({"action": "set", "pin": "PC7", "value": 0})
        self.assertEqual(value["type"], "result")
        denied = await self.send({"action": "mode", "pin": "PB5", "mode": "in"})
        self.assertEqual(denied["type"], "error")
        disconnected = await self.send({"action": "disconnect"})
        self.assertEqual(disconnected["type"], "disconnected")

    async def test_dangerous_output_requires_confirmation(self):
        await self.send({"action": "connect"})
        prompt = await self.send({"action": "mode", "pin": "PA1", "mode": "hi"})
        self.assertEqual(prompt["type"], "confirm_required")
        confirmed = await self.send(
            {"action": "mode", "pin": "PA1", "mode": "hi", "confirm": "PA1"}
        )
        self.assertEqual(confirmed["type"], "result")


if __name__ == "__main__":
    unittest.main()
