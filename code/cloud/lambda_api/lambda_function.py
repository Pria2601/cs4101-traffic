"""
Lambda: traffic_api
Trigger: API Gateway (HTTP API)
  GET  /state    -> latest reading, recent history, latest prediction, recent alerts
  POST /command  -> send a control command to the device (needs the admin PIN)

POST body examples:
  {"pin":"1234","cmd":"mode","mode":"AUTO"}
  {"pin":"1234","cmd":"mode","mode":"MANUAL","lane":2}
  {"pin":"1234","cmd":"mode","mode":"ALLRED"}
  {"pin":"1234","cmd":"set","key":"MAXGREEN","value":25}
  {"pin":"1234","cmd":"reset"}
"""
import json
import os
import time
from decimal import Decimal

import boto3
from boto3.dynamodb.conditions import Key

REGION = os.environ.get("REGION", "ap-south-1")
DEVICE = os.environ.get("DEVICE_ID", "traffic_node_1")
READINGS_TABLE = os.environ.get("READINGS_TABLE", "TrafficReadings")
PREDICTIONS_TABLE = os.environ.get("PREDICTIONS_TABLE", "TrafficPredictions")
ALERTS_TABLE = os.environ.get("ALERTS_TABLE", "TrafficAlerts")
IOT_DATA_ENDPOINT = os.environ.get("IOT_DATA_ENDPOINT", "")
COMMAND_TOPIC = os.environ.get("COMMAND_TOPIC", "traffic/sub")
CONTROL_PIN = os.environ.get("CONTROL_PIN", "")
SNS_TOPIC_ARN = os.environ.get("SNS_TOPIC_ARN", "")
HISTORY_POINTS = int(os.environ.get("HISTORY_POINTS", "60"))

dynamodb = boto3.resource("dynamodb", region_name=REGION)
readings = dynamodb.Table(READINGS_TABLE)
predictions = dynamodb.Table(PREDICTIONS_TABLE)
alerts = dynamodb.Table(ALERTS_TABLE)
iot = boto3.client("iot-data", region_name=REGION, endpoint_url=f"https://{IOT_DATA_ENDPOINT}")
sns = boto3.client("sns", region_name=REGION)

SET_KEYS = {  # key -> (min, max), same limits as the Mega
    "WQ": (0, 10), "WW": (0, 10), "MINGREEN": (3, 60),
    "MAXGREEN": (5, 120), "PERVEH": (0, 10), "MAXWAIT": (10, 300),
}
HEADERS = {
    "Content-Type": "application/json",
    "Access-Control-Allow-Origin": "*",
    "Access-Control-Allow-Headers": "content-type",
    "Access-Control-Allow-Methods": "GET,POST,OPTIONS",
}


def to_plain(obj):
    if isinstance(obj, list):
        return [to_plain(o) for o in obj]
    if isinstance(obj, dict):
        return {k: to_plain(v) for k, v in obj.items()}
    if isinstance(obj, Decimal):
        return int(obj) if obj == obj.to_integral_value() else float(obj)
    return obj


def reply(status, body):
    return {"statusCode": status, "headers": HEADERS, "body": json.dumps(to_plain(body))}


def latest(table, limit):
    resp = table.query(
        KeyConditionExpression=Key("device").eq(DEVICE),
        ScanIndexForward=False,
        Limit=limit,
    )
    return resp.get("Items", [])


def get_state():
    hist = list(reversed(latest(readings, HISTORY_POINTS)))
    pred = latest(predictions, 1)
    return {
        "device": DEVICE,
        "server_time": int(time.time() * 1000),
        "latest": hist[-1] if hist else None,
        "history": [
            {k: r.get(k) for k in ("ts", "q1", "q2", "q3", "q4", "lights", "green")}
            for r in hist
        ],
        "prediction": pred[0] if pred else None,
        "alerts": latest(alerts, 10),
    }


def build_command(body):
    """Validate the request and return the MQTT command (or raise ValueError)."""
    cmd = str(body.get("cmd", "")).lower()
    if cmd == "mode":
        mode = str(body.get("mode", "")).upper()
        if mode in ("AUTO", "ALLRED"):
            return {"cmd": "mode", "mode": mode}
        if mode == "MANUAL":
            lane = int(body.get("lane", 0))
            if 1 <= lane <= 4:
                return {"cmd": "mode", "mode": "MANUAL", "lane": lane}
            raise ValueError("lane must be 1-4")
        raise ValueError("mode must be AUTO, MANUAL or ALLRED")
    if cmd == "set":
        key = str(body.get("key", "")).upper()
        if key not in SET_KEYS:
            raise ValueError(f"key must be one of {', '.join(SET_KEYS)}")
        value = float(body.get("value"))
        lo, hi = SET_KEYS[key]
        if not lo <= value <= hi:
            raise ValueError(f"{key} must be between {lo} and {hi}")
        return {"cmd": "set", "key": key, "value": value}
    if cmd in ("reset", "ping"):
        return {"cmd": cmd}
    raise ValueError("unknown cmd")


def notify_override(command):
    """Email the admin when the signal is taken out of AUTO mode."""
    if command.get("cmd") != "mode" or command["mode"] == "AUTO":
        return
    if command["mode"] == "ALLRED":
        subject = "Traffic alert: EMERGENCY all-red activated"
        text = "All four signals were set to RED from the dashboard (emergency mode)."
        lane = 0
    else:
        lane = command["lane"]
        subject = f"Traffic alert: manual override, Lane {lane} green"
        text = f"Lane {lane} was given a fixed green from the dashboard (manual mode)."
    if SNS_TOPIC_ARN:
        sns.publish(TopicArn=SNS_TOPIC_ARN, Subject=subject, Message=text)
    alerts.put_item(Item={
        "device": DEVICE, "ts": int(time.time() * 1000), "key": f"override-L{lane}",
        "kind": "override", "lane": lane, "subject": subject, "message": text,
    })


def send_command(raw_body):
    try:
        body = json.loads(raw_body or "{}")
    except json.JSONDecodeError:
        return reply(400, {"ok": False, "error": "body must be JSON"})

    if not CONTROL_PIN or str(body.get("pin", "")) != CONTROL_PIN:
        return reply(403, {"ok": False, "error": "wrong PIN"})
    try:
        command = build_command(body)
    except (ValueError, TypeError) as e:
        return reply(400, {"ok": False, "error": str(e)})

    iot.publish(topic=COMMAND_TOPIC, qos=1, payload=json.dumps(command))
    notify_override(command)
    print(f"Command sent: {command}")
    return reply(200, {"ok": True, "sent": command})


def lambda_handler(event, context):
    http = event.get("requestContext", {}).get("http", {})
    method = http.get("method", event.get("httpMethod", "GET"))
    path = event.get("rawPath", event.get("path", "/"))

    if method == "OPTIONS":
        return reply(200, {"ok": True})
    if method == "GET" and path.endswith("/state"):
        return reply(200, get_state())
    if method == "POST" and path.endswith("/command"):
        return send_command(event.get("body"))
    return reply(404, {"ok": False, "error": f"no route {method} {path}"})
