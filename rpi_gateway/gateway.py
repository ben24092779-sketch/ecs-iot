#!/usr/bin/env python3
import json
import logging
import os
import ssl
import time
from typing import Optional

import paho.mqtt.client as mqtt
import requests

LOG = logging.getLogger("ender-rpi-gateway")

DEVICE_SLUG = os.getenv("DEVICE_SLUG", "ender-rpi")
DEVICE_TOKEN = os.environ["DEVICE_TOKEN"]
MQTT_HOST = os.environ["MQTT_HOST"]
MQTT_PORT = int(os.getenv("MQTT_PORT", "8883"))
MQTT_USER = os.environ["MQTT_USER"]
MQTT_PASSWORD = os.environ["MQTT_PASSWORD"]
OCTOPRINT_URL = os.getenv("OCTOPRINT_URL", "http://127.0.0.1").rstrip("/")
OCTOPRINT_API_KEY = os.environ["OCTOPRINT_API_KEY"]
POLL_SECONDS = float(os.getenv("POLL_SECONDS", "3"))
REMOTE_CONTROL_ENABLED = os.getenv("REMOTE_CONTROL_ENABLED", "0") == "1"

session = requests.Session()
session.headers.update({"X-Api-Key": OCTOPRINT_API_KEY, "Content-Type": "application/json"})


def topic(variable: str) -> str:
    return f"ecs/{DEVICE_SLUG}/{variable}"


def publish_value(client, variable: str, value: float):
    payload = json.dumps({"value": float(value), "token": DEVICE_TOKEN}, separators=(",", ":"))
    info = client.publish(topic(variable), payload=payload, qos=1, retain=False)
    if info.rc != mqtt.MQTT_ERR_SUCCESS:
        LOG.warning("MQTT publish failed for %s (rc=%s)", variable, info.rc)


def octo_get(path: str):
    r = session.get(OCTOPRINT_URL + path, timeout=5)
    r.raise_for_status()
    return r.json()


def octo_post(path: str, body: dict):
    r = session.post(OCTOPRINT_URL + path, data=json.dumps(body), timeout=5)
    if r.status_code not in (200, 204):
        raise RuntimeError(f"OctoPrint {path}: HTTP {r.status_code} {r.text[:200]}")


def state_code(text: str) -> int:
    s = (text or "").lower()
    if "offline" in s or "disconnected" in s or "closed" in s:
        return 0
    if "error" in s:
        return 4
    if "paused" in s:
        return 3
    if any(x in s for x in ("printing", "pausing", "cancelling", "starting", "finishing")):
        return 2
    return 1


def valid_number(value) -> Optional[float]:
    return float(value) if isinstance(value, (int, float)) else None


def publish_telemetry(client):
    try:
        printer = octo_get("/api/printer")
        job = octo_get("/api/job")

        tool0 = printer.get("temperature", {}).get("tool0", {})
        bed = printer.get("temperature", {}).get("bed", {})
        state_text = printer.get("state", {}).get("text") or job.get("state", "")

        values = {
            "temp_hotend": valid_number(tool0.get("actual")),
            "sp_hotend": valid_number(tool0.get("target")),
            "temp_bed": valid_number(bed.get("actual")),
            "sp_bed": valid_number(bed.get("target")),
            "printer_state": state_code(state_text),
            "print_progress": valid_number(job.get("progress", {}).get("completion")) or 0.0,
            "print_time_s": valid_number(job.get("progress", {}).get("printTime")) or 0.0,
            "raspberry_online": 1.0,
        }
        for variable, value in values.items():
            if value is not None:
                publish_value(client, variable, value)
        LOG.info(
            "Telemetry OK: state=%s hotend=%s bed=%s progress=%s",
            state_text, values["temp_hotend"], values["temp_bed"], values["print_progress"]
        )
    except requests.RequestException as exc:
        LOG.warning("OctoPrint unavailable: %s", exc)
        publish_value(client, "printer_state", 0)
        publish_value(client, "raspberry_online", 1)


def command_allowed(variable: str, value: float):
    if variable in {"pause", "resume", "cancel", "print", "home_xyz"}:
        if value != 1:
            raise ValueError("button command requires value=1")
    elif variable in {"jog_x", "jog_y", "jog_z"}:
        if abs(value) > 10:
            raise ValueError("jog limited to ±10 mm")
    elif variable == "sp_hotend":
        if not 0 <= value <= 260:
            raise ValueError("hotend target outside 0..260 C")
    elif variable == "sp_bed":
        if not 0 <= value <= 110:
            raise ValueError("bed target outside 0..110 C")
    else:
        raise ValueError("unsupported command")


def execute_command(variable: str, value: float):
    command_allowed(variable, value)
    if variable == "pause":
        octo_post("/api/job", {"command": "pause", "action": "pause"})
    elif variable == "resume":
        octo_post("/api/job", {"command": "pause", "action": "resume"})
    elif variable == "cancel":
        octo_post("/api/job", {"command": "cancel"})
    elif variable == "print":
        octo_post("/api/job", {"command": "start"})
    elif variable == "home_xyz":
        octo_post("/api/printer/printhead", {"command": "home", "axes": ["x", "y", "z"]})
    elif variable in {"jog_x", "jog_y", "jog_z"}:
        axis = variable[-1]
        octo_post("/api/printer/printhead", {"command": "jog", axis: value})
    elif variable == "sp_hotend":
        octo_post("/api/printer/tool", {"command": "target", "targets": {"tool0": value}})
    elif variable == "sp_bed":
        octo_post("/api/printer/bed", {"command": "target", "target": value})


def on_connect(client, userdata, flags, rc, properties=None):
    if rc == 0:
        LOG.info("Connected to EMQX")
        client.subscribe(f"ecs/{DEVICE_SLUG}/+/set", qos=1)
        publish_value(client, "raspberry_online", 1)
    else:
        LOG.error("MQTT connection failed rc=%s", rc)


def on_message(client, userdata, msg):
    try:
        parts = msg.topic.split("/")
        if len(parts) != 4 or parts[0] != "ecs" or parts[1] != DEVICE_SLUG or parts[3] != "set":
            return
        variable = parts[2]
        data = json.loads(msg.payload.decode("utf-8"))
        if str(data.get("token", "")) != DEVICE_TOKEN:
            LOG.warning("Rejected command %s: invalid token", variable)
            return
        value = float(data["value"])
        if not REMOTE_CONTROL_ENABLED:
            LOG.warning("Rejected command %s: REMOTE_CONTROL_ENABLED=0", variable)
            return
        execute_command(variable, value)
        LOG.info("Command OK: %s=%s", variable, value)
        time.sleep(0.2)
        publish_telemetry(client)
    except Exception as exc:
        LOG.exception("Command failed on %s: %s", msg.topic, exc)


def build_client():
    try:
        client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f"{DEVICE_SLUG}-gateway")
    except AttributeError:
        client = mqtt.Client(client_id=f"{DEVICE_SLUG}-gateway")
    client.username_pw_set(MQTT_USER, MQTT_PASSWORD)
    context = ssl.create_default_context()
    client.tls_set_context(context)
    client.tls_insecure_set(False)
    offline = json.dumps({"value": 0, "token": DEVICE_TOKEN}, separators=(",", ":"))
    client.will_set(topic("raspberry_online"), payload=offline, qos=1, retain=False)
    client.on_connect = on_connect
    client.on_message = on_message
    client.reconnect_delay_set(min_delay=1, max_delay=30)
    return client


def main():
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    client = build_client()
    client.connect(MQTT_HOST, MQTT_PORT, keepalive=60)
    client.loop_start()
    try:
        while True:
            if client.is_connected():
                publish_telemetry(client)
            time.sleep(POLL_SECONDS)
    finally:
        try:
            publish_value(client, "raspberry_online", 0)
        except Exception:
            pass
        client.loop_stop()
        client.disconnect()


if __name__ == "__main__":
    main()
