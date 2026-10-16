"""
Lambda: traffic_infer
Trigger: IoT rule on topic traffic/pub (runs for EVERY sensor message)

Job:
  1. Take the new reading + the previous few readings from DynamoDB
  2. Run the trained congestion model (from S3) for each lane
  3. Save the prediction to the TrafficPredictions table (dashboard shows it)
  4. Send an SNS email alert for HIGH congestion or a faulty sensor

Files in this Lambda: lambda_function.py, traffic_ml.py
Environment variables: see docs/AWS_SETUP.md
"""
import json
import os
import time
from decimal import Decimal

import boto3
from boto3.dynamodb.conditions import Key

import traffic_ml as ml

REGION = os.environ.get("REGION", "ap-south-1")
READINGS_TABLE = os.environ.get("READINGS_TABLE", "TrafficReadings")
PREDICTIONS_TABLE = os.environ.get("PREDICTIONS_TABLE", "TrafficPredictions")
ALERTS_TABLE = os.environ.get("ALERTS_TABLE", "TrafficAlerts")
MODEL_BUCKET = os.environ.get("MODEL_BUCKET", "")
MODEL_KEY = os.environ.get("MODEL_KEY", "models/congestion_model.json")
SNS_TOPIC_ARN = os.environ.get("SNS_TOPIC_ARN", "")
ALERT_COOLDOWN_S = int(os.environ.get("ALERT_COOLDOWN_S", "300"))
DASHBOARD_URL = os.environ.get("DASHBOARD_URL", "")

dynamodb = boto3.resource("dynamodb", region_name=REGION)
readings = dynamodb.Table(READINGS_TABLE)
predictions = dynamodb.Table(PREDICTIONS_TABLE)
alerts = dynamodb.Table(ALERTS_TABLE)
s3 = boto3.client("s3", region_name=REGION)
sns = boto3.client("sns", region_name=REGION)

_model = None
_model_loaded_at = 0.0


def get_model():
    """Load model JSON from S3, re-check every 5 minutes so a new upload is picked up."""
    global _model, _model_loaded_at
    if time.time() - _model_loaded_at < 300:
        return _model
    _model_loaded_at = time.time()
    try:
        body = s3.get_object(Bucket=MODEL_BUCKET, Key=MODEL_KEY)["Body"].read()
        _model = json.loads(body)
        print(f"Model loaded: {_model.get('name')} trained {_model.get('trained_at')}")
    except Exception as e:  # no model yet -> rule-based fallback
        print(f"Model not loaded ({e}); using rule fallback")
        _model = None
    return _model


def previous_readings(device, ts):
    resp = readings.query(
        KeyConditionExpression=Key("device").eq(device) & Key("ts").lt(ts),
        ScanIndexForward=False,
        Limit=ml.HISTORY,
    )
    return list(reversed(resp.get("Items", [])))


def send_alert(device, ts, kind, lane, subject, message):
    """Send SNS email unless the same alert was sent within the cooldown."""
    key = f"{kind}-L{lane}"
    recent = alerts.query(
        KeyConditionExpression=Key("device").eq(device) & Key("ts").gt(ts - ALERT_COOLDOWN_S * 1000),
    ).get("Items", [])
    if any(a.get("key") == key for a in recent):
        return False

    if DASHBOARD_URL:
        message += f"\n\nDashboard: {DASHBOARD_URL}"
    if SNS_TOPIC_ARN:
        sns.publish(TopicArn=SNS_TOPIC_ARN, Subject=subject[:99], Message=message)
    # small offset keeps two alerts from the same reading from overwriting each other
    alert_ts = ts + lane + (5 if kind == "fault" else 0)
    alerts.put_item(Item={
        "device": device, "ts": alert_ts, "key": key, "kind": kind,
        "lane": lane, "subject": subject, "message": message,
    })
    print(f"ALERT sent: {subject}")
    return True


def lambda_handler(event, context):
    device = event.get("device", "traffic_node_1")
    ts = int(event.get("ts", time.time() * 1000))

    rows = previous_readings(device, ts) + [event]
    model = get_model()

    item = {
        "device": device,
        "ts": ts,
        "source": model.get("name", "model") if model else "rule",
    }
    for lane in ml.LANES:
        x = ml.lane_features(rows, lane)
        lvl, probs = ml.predict(model, x) if model else ml.predict_rule(x)
        item[f"p{lane}"] = ml.LEVELS[lvl]
        item[f"c{lane}"] = Decimal(str(round(probs[lvl], 3)))

        queue = int(ml.num(event, f"q{lane}"))
        wait_s = int(ml.num(event, f"w{lane}"))
        if lvl == 2 and queue > ml.LOW_MAX:
            send_alert(
                device, ts, "congestion", lane,
                f"Traffic alert: HIGH congestion at Lane {lane}",
                f"Lane {lane} is predicted to stay HIGH for the next ~30 s.\n"
                f"Vehicles waiting: {queue}\nFirst vehicle waiting: {wait_s} s\n"
                f"Model confidence: {probs[lvl]:.0%}",
            )
        if int(ml.num(event, f"f{lane}")) == 1:
            send_alert(
                device, ts, "fault", lane,
                f"Traffic alert: IR sensor fault at Lane {lane}",
                f"The IR sensor of Lane {lane} has been blocked for more than 10 s.\n"
                f"Please check the sensor.",
            )

    predictions.put_item(Item=item)
    return {"ok": True, "prediction": {k: str(v) for k, v in item.items()}}
