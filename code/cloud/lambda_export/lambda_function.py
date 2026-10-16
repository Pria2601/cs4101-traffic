"""
Lambda: traffic_export
Trigger: run manually (Test button) when you want a fresh training dataset.

Job: read all readings of the device from DynamoDB and save them as a CSV
     in S3 -> s3://<DATA_BUCKET>/training-data/traffic_readings.csv
"""
import csv
import io
import os

import boto3
from boto3.dynamodb.conditions import Key

REGION = os.environ.get("REGION", "ap-south-1")
DEVICE = os.environ.get("DEVICE_ID", "traffic_node_1")
READINGS_TABLE = os.environ.get("READINGS_TABLE", "TrafficReadings")
DATA_BUCKET = os.environ.get("DATA_BUCKET", "")
CSV_KEY = os.environ.get("CSV_KEY", "training-data/traffic_readings.csv")

COLUMNS = (
    ["device", "ts", "seq", "up", "mode", "ml", "green", "phase", "rem", "rsn", "lights"]
    + [f"{p}{l}" for l in range(1, 5) for p in ("q", "w", "a", "t", "s", "f")]
    + ["wq", "ww", "ming", "maxg", "pv", "mw"]
)

table = boto3.resource("dynamodb", region_name=REGION).Table(READINGS_TABLE)
s3 = boto3.client("s3", region_name=REGION)


def lambda_handler(event, context):
    items, kwargs = [], {"KeyConditionExpression": Key("device").eq(DEVICE)}
    while True:
        resp = table.query(**kwargs)
        items.extend(resp.get("Items", []))
        if "LastEvaluatedKey" not in resp:
            break
        kwargs["ExclusiveStartKey"] = resp["LastEvaluatedKey"]

    out = io.StringIO()
    writer = csv.DictWriter(out, fieldnames=COLUMNS, extrasaction="ignore")
    writer.writeheader()
    for item in items:
        writer.writerow({k: item.get(k, "") for k in COLUMNS})

    s3.put_object(Bucket=DATA_BUCKET, Key=CSV_KEY, Body=out.getvalue(), ContentType="text/csv")
    msg = f"Exported {len(items)} readings to s3://{DATA_BUCKET}/{CSV_KEY}"
    print(msg)
    return {"ok": True, "rows": len(items), "message": msg}
