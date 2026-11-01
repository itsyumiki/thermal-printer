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
        self.assertEqual(hello["cfg_hash"], "ea4a6cb8")

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

    async def test_startup_macro_latches_power_on_and_prepares_button_input(self):
        await self.send({"action": "connect"})
        result = await self.send({"action": "startup"})
        self.assertEqual(result["type"], "result")
        self.assertEqual(result["action"], "startup")
        pins = {p["name"]: p for p in result["status"]["pins"]}
        self.assertEqual(pins["PA1"]["mode"], "hi")
        self.assertEqual(pins["PA1"]["value"], 1)
        self.assertEqual(pins["PB1"]["mode"], "up")

    async def test_read_button_pin_prepares_pullup_before_get(self):
        await self.send({"action": "connect"})
        result = await self.send({"action": "get", "pin": "PB1"})
        self.assertEqual(result["type"], "result")
        self.assertIn("PB1=1", result["reply"][-1])
        self.assertEqual(
            {p["name"]: p["mode"] for p in result["status"]["pins"]}["PB1"], "up"
        )

    async def test_printer_poll_translates_digital_sensors_and_reads_adc(self):
        await self.send({"action": "connect"})
        result = await self.send({"action": "printer_poll"})
        self.assertEqual(result["type"], "printer_status")
        printer = result["printer"]
        self.assertTrue(printer["paper_present"])
        self.assertFalse(printer["power_button_pressed"])
        self.assertEqual(printer["head_sensor"], {"raw": 512, "mv": 1650})
        self.assertEqual(printer["battery_sensor"], {"raw": 620, "mv": 2000})
        modes = {p["name"]: p["mode"] for p in result["status"]["pins"]}
        self.assertEqual(modes["PC4"], "adc")
        self.assertEqual(modes["PC5"], "adc")

    async def test_adc_read_action(self):
        await self.send({"action": "connect"})
        result = await self.send({"action": "adc", "pin": "PC4"})
        self.assertEqual(result["type"], "result")
        self.assertEqual(result["reply"], ["PC4=512/1650"])
        pin = next(p for p in result["status"]["pins"] if p["name"] == "PC4")
        self.assertEqual((pin["value"], pin["mv"]), (512, 1650))

    async def test_power_button_action_sets_pa1_low_and_high(self):
        await self.send({"action": "connect"})
        on = await self.send({"action": "power", "value": 1})
        self.assertEqual(on["type"], "result")
        self.assertEqual(
            {p["name"]: p["value"] for p in on["status"]["pins"]}["PA1"], 1
        )
        off = await self.send({"action": "power", "value": 0})
        self.assertEqual(off["type"], "result")
        self.assertEqual(
            {p["name"]: p["value"] for p in off["status"]["pins"]}["PA1"], 0
        )

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
