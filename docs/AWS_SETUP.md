# AWS Setup

## ⚡ Quick way (recommended): one script in CloudShell
1. Log in to the AWS console and set the region (top-right) to **Asia Pacific (Mumbai)**.
2. Open **CloudShell**: the `>_` icon in the top bar.
3. **Actions → Upload file** → `code/deploy/traffic_aws_bundle.zip`.
4. Run:
   ```
   unzip -o traffic_aws_bundle.zip -d traffic && cd traffic
   bash setup.sh your@email.com 4821
   ```
   Use your alert email and an admin PIN of 4–8 digits.
5. Confirm the SNS email in your inbox (check spam too).
6. Test without hardware: `bash fake_device.sh`. Then open the dashboard URL printed by the setup script.
7. **Actions → Download file** → `traffic/out/secrets.h`. Fill in the WiFi name and password, then give it to the teammate privately.

The script creates everything in Steps 1–10 below. Running it again is safe: it reuses or updates what already exists.
If you change any cloud or dashboard code, rebuild the zip, upload it again and re-run the script.

The manual steps below show what the script does (useful for the report and for console screenshots).

---

# Manual steps (reference; follows sir's AWS_tutorial.pdf)

Use **one region for everything: Asia Pacific (Mumbai) `ap-south-1`.**
Replace `<name>` with something unique (e.g. your roll number).
Keep a notes file with: account ID, IoT endpoint, bucket names, SNS ARN, API URL, PIN.

---

## Step 1 – IoT Core policy (slide 4)
IoT Core → Security → Policies → **Create policy** → name `TrafficNodePolicy` → JSON editor.
Replace `ACCOUNT_ID` with your 12-digit account ID (top-right menu):

```json
{
  "Version": "2012-10-17",
  "Statement": [
    { "Effect": "Allow", "Action": "iot:Connect",
      "Resource": "arn:aws:iot:ap-south-1:ACCOUNT_ID:client/traffic_node_1" },
    { "Effect": "Allow", "Action": "iot:Publish",
      "Resource": ["arn:aws:iot:ap-south-1:ACCOUNT_ID:topic/traffic/pub",
                   "arn:aws:iot:ap-south-1:ACCOUNT_ID:topic/traffic/ack"] },
    { "Effect": "Allow", "Action": "iot:Subscribe",
      "Resource": "arn:aws:iot:ap-south-1:ACCOUNT_ID:topicfilter/traffic/sub" },
    { "Effect": "Allow", "Action": "iot:Receive",
      "Resource": "arn:aws:iot:ap-south-1:ACCOUNT_ID:topic/traffic/sub" }
  ]
}
```

## Step 2 – Thing + certificate (slide 5)
IoT Core → All devices → Things → **Create things** → One thing → name **`traffic_node_1`** → No shadow →
**Auto-generate a new certificate** → attach `TrafficNodePolicy` → Create.

**Download now** (only offered once): device certificate, private key, Amazon Root CA 1.

Open `code/nodemcu_gateway/secrets.h` and paste:
- the contents of `AmazonRootCA1.pem` → `AWS_CERT_CA`
- `...-certificate.pem.crt` → `AWS_CERT_CRT`
- `...-private.pem.key` → `AWS_CERT_PRIVATE`

Replace everything between `R"EOF(` and `)EOF"`, including the BEGIN/END lines.

Endpoint: IoT Core → **Domain configurations** (or Settings) → copy the domain name → `AWS_IOT_ENDPOINT`.
Also fill in your WiFi name and password. It must be a **2.4 GHz** network; a phone hotspot is fine.

## Step 3 – DynamoDB tables (slide 7)
DynamoDB → Tables → **Create table** three times. Leave everything else at default.

| Table name | Partition key | Sort key |
|---|---|---|
| `TrafficReadings` | `device` (String) | `ts` (Number) |
| `TrafficPredictions` | `device` (String) | `ts` (Number) |
| `TrafficAlerts` | `device` (String) | `ts` (Number) |

## Step 4 – SNS email (slide 14)
SNS → Topics → **Create topic** → Standard → name `TrafficAlerts` → Create. Copy the **ARN**.
Create subscription → Protocol **Email** → your email → then **confirm from your inbox**.

## Step 5 – S3 buckets
1. **Data bucket** (private): create `traffic-data-<name>`. Inside it, create folders `training-data/` and `models/`.
2. **Dashboard bucket** (public website): create `traffic-dashboard-<name>`.
   - Untick "Block all public access" and acknowledge.
   - Properties → Static website hosting → Enable → index document `index.html`.
   - Permissions → Bucket policy:
   ```json
   { "Version": "2012-10-17", "Statement": [{ "Effect": "Allow", "Principal": "*",
     "Action": "s3:GetObject", "Resource": "arn:aws:s3:::traffic-dashboard-<name>/*" }] }
   ```
   - Copy the **website endpoint URL**. It is used in steps 6 and 8.

## Step 6 – Lambda `traffic_infer` (live ML + alerts)
Lambda → Create function → name `traffic_infer` → Python 3.13 → Create.
- Code: paste `code/cloud/lambda_infer/lambda_function.py`. Then File → New file → `traffic_ml.py` and paste `code/cloud/lambda_infer/traffic_ml.py`. Click **Deploy**.
- Configuration → General → Timeout **15 s**.
- Configuration → Environment variables:

| Key | Value |
|---|---|
| `MODEL_BUCKET` | `traffic-data-<name>` |
| `SNS_TOPIC_ARN` | ARN from Step 4 |
| `DASHBOARD_URL` | website URL from Step 5 (optional) |

- Configuration → Permissions → role name → Add permissions → **Create inline policy** → JSON:
```json
{ "Version": "2012-10-17", "Statement": [
  { "Effect": "Allow", "Action": ["dynamodb:Query", "dynamodb:PutItem"],
    "Resource": ["arn:aws:dynamodb:ap-south-1:*:table/TrafficReadings",
                 "arn:aws:dynamodb:ap-south-1:*:table/TrafficPredictions",
                 "arn:aws:dynamodb:ap-south-1:*:table/TrafficAlerts"] },
  { "Effect": "Allow", "Action": "s3:GetObject", "Resource": "arn:aws:s3:::traffic-data-<name>/models/*" },
  { "Effect": "Allow", "Action": "sns:Publish", "Resource": "SNS_TOPIC_ARN" } ] }
```

## Step 7 – IoT rule (slide 8): store every reading + run the model
IoT Core → Message routing → Rules → **Create rule** → name `TrafficIngestRule`.
- SQL version `2016-03-23`:
  ```sql
  SELECT *, timestamp() AS ts FROM 'traffic/pub'
  ```
- Action 1: **DynamoDBv2** → table `TrafficReadings` → IAM role → Create new role `TrafficIoTRuleRole`.
- Action 2: **Lambda** → `traffic_infer`.
- Create.

## Step 8 – Lambda `traffic_api` + API Gateway (slide 16)
Lambda → Create function `traffic_api` (Python 3.13) → paste `code/cloud/lambda_api/lambda_function.py` → Deploy. Set Timeout to 10 s.

Environment variables:

| Key | Value |
|---|---|
| `IOT_DATA_ENDPOINT` | same endpoint as in `secrets.h` |
| `CONTROL_PIN` | a PIN you choose, e.g. `4821` |
| `SNS_TOPIC_ARN` | ARN from Step 4 |

Inline policy:
```json
{ "Version": "2012-10-17", "Statement": [
  { "Effect": "Allow", "Action": ["dynamodb:Query", "dynamodb:PutItem"],
    "Resource": ["arn:aws:dynamodb:ap-south-1:*:table/TrafficReadings",
                 "arn:aws:dynamodb:ap-south-1:*:table/TrafficPredictions",
                 "arn:aws:dynamodb:ap-south-1:*:table/TrafficAlerts"] },
  { "Effect": "Allow", "Action": "iot:Publish", "Resource": "arn:aws:iot:ap-south-1:*:topic/traffic/sub" },
  { "Effect": "Allow", "Action": "sns:Publish", "Resource": "SNS_TOPIC_ARN" } ] }
```

API Gateway → Create API → **HTTP API** → Build:
- Integration: Lambda `traffic_api`. Name `TrafficAPI`.
- Routes: `GET /state` and `POST /command`, both pointing to the `traffic_api` integration.
- Stage `$default` with auto-deploy → Create.
- **CORS**: Allow-Origin = your dashboard website URL (or `*`), Allow-Methods = `GET, POST, OPTIONS`, Allow-Headers = `content-type`. Note that sir's slide shows only GET, but we also need **POST**.
- Copy the **Invoke URL**.

## Step 9 – Dashboard
In `code/dashboard/index.html`, set `const API_BASE = "<Invoke URL>"` (no trailing `/`).
Upload `index.html` to the dashboard bucket, then open the website URL.

## Step 10 – Lambda `traffic_export` (dataset to S3, slide 12)
Lambda → Create function `traffic_export` (Python 3.13) → paste `code/cloud/lambda_export/lambda_function.py` → Deploy.
- Timeout **60 s**. Env var `DATA_BUCKET` = `traffic-data-<name>`.
- Inline policy:
```json
{ "Version": "2012-10-17", "Statement": [
  { "Effect": "Allow", "Action": "dynamodb:Query", "Resource": "arn:aws:dynamodb:ap-south-1:*:table/TrafficReadings" },
  { "Effect": "Allow", "Action": "s3:PutObject", "Resource": "arn:aws:s3:::traffic-data-<name>/training-data/*" } ] }
```
- Run it with Test → event `{}` once your data is collected.

## Step 11 – Train and deploy the model (after real data is collected)
1. Run `traffic_export` → Test with `{}` → the CSV appears in S3 under `training-data/`.
2. Download it to `code/ml/data/traffic_readings.csv`.
3. In the `code/ml` folder, run:
   ```
   pip install pandas scikit-learn matplotlib
   python train_model.py --csv data/traffic_readings.csv
   ```
4. Check `output/report.txt`. The model should beat the "Baseline (no change)" row on macro-F1.
5. Upload `output/congestion_model.json` to S3 at `models/congestion_model.json`.
6. Within 5 minutes, the dashboard's "Forecast model" changes from "Rule (no model yet)" to the model name.

---

## Testing without hardware (MQTT test client acts as the NodeMCU)
IoT Core → **MQTT test client**.

**A. Watch commands.** Subscribe to `traffic/sub`.

**B. Send a fake reading.** Publish to topic `traffic/pub` with the message below. Change `seq`, `up` and the `q`/`w`/`a` numbers each time you publish.
```json
{"device":"traffic_node_1","seq":1,"up":10,"mode":"AUTO","ml":0,"green":2,"phase":"G","rem":7,"rsn":"score","lights":"RGRR",
 "q1":4,"w1":18,"a1":1,"t1":20,"s1":17.0,"f1":0,
 "q2":1,"w2":0,"a2":2,"t2":31,"s2":2.0,"f2":0,
 "q3":0,"w3":0,"a3":0,"t3":9,"s3":0.0,"f3":0,
 "q4":7,"w4":25,"a4":2,"t4":15,"s4":26.5,"f4":0,
 "wq":2.0,"ww":0.5,"ming":5,"maxg":20,"pv":2,"mw":40}
```

| Check | Expected |
|---|---|
| DynamoDB → TrafficReadings | New row with a `ts` |
| DynamoDB → TrafficPredictions | Row with `p1..p4` (source `rule` until a model is uploaded) |
| Email | `q4` = 7 (above 5) → "HIGH congestion at Lane 4" email |
| Set `"f2":1` and publish again | "IR sensor fault at Lane 2" email |
| Dashboard | Shows the fake reading (it says "offline" about 15 s after your last publish, which is normal) |
| Dashboard button "Lane 3 green" + PIN | `{"cmd":"mode","mode":"MANUAL","lane":3}` appears on `traffic/sub`, plus an override email |
| Lambda → Monitor → CloudWatch logs | No errors |

If all of these pass, the cloud side is done. When the hardware is connected, the real NodeMCU simply replaces the test client.

---

### Troubleshooting
| Problem | Check |
|---|---|
| NodeMCU `[AWS] Failed, state=-2` | Endpoint wrong, certificate not attached to the policy, or time sync failed |
| Connects then drops repeatedly | Client ID must be `traffic_node_1` (it must match the policy) |
| Nothing in DynamoDB | Rule SQL topic is `'traffic/pub'`; check the rule's IAM role |
| Dashboard says "API unreachable" | `API_BASE` URL, CORS settings, routes deployed |
| Command "wrong PIN" | `CONTROL_PIN` env var on `traffic_api` |
| No emails | Subscription not confirmed; Lambda env var `SNS_TOPIC_ARN` |
| Lambda errors | Lambda → Monitor → View CloudWatch logs |
