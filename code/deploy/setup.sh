#!/usr/bin/env bash
# =====================================================================
# CS4101 Smart Traffic - one-shot AWS setup
# Run in AWS CloudShell (region Asia Pacific Mumbai):
#     unzip traffic_aws_bundle.zip -d traffic && cd traffic
#     bash setup.sh <alert-email> <admin-pin>
# Safe to run again: existing resources are reused / updated.
# =====================================================================
set -euo pipefail

EMAIL="${1:-}"
PIN="${2:-}"
if [[ ! "$EMAIL" =~ ^[^@[:space:]]+@[^@[:space:]]+\.[^@[:space:]]+$ ]] || [[ ! "$PIN" =~ ^[0-9]{4,8}$ ]]; then
  echo "Usage: bash setup.sh <alert-email> <admin-pin (4-8 digits)>"
  exit 1
fi

export AWS_DEFAULT_REGION=ap-south-1
export AWS_PAGER=""
REGION=ap-south-1
DIR="$(cd "$(dirname "$0")" && pwd)"
OUT="$DIR/out"
mkdir -p "$OUT"

ACCOUNT=$(aws sts get-caller-identity --query Account --output text)
THING=traffic_node_1
DATA_BUCKET="traffic-data-$ACCOUNT"
WEB_BUCKET="traffic-dashboard-$ACCOUNT"
WEB_URL="http://$WEB_BUCKET.s3-website.$REGION.amazonaws.com"
TABLE_ARN="arn:aws:dynamodb:$REGION:$ACCOUNT:table"

step() { echo; echo "=== $* ==="; }

# ---------------------------------------------------------------------
step "1/9 IoT Core: policy, thing, certificate"
if ! aws iot get-policy --policy-name TrafficNodePolicy >/dev/null 2>&1; then
  aws iot create-policy --policy-name TrafficNodePolicy --policy-document "$(cat <<EOF
{
  "Version": "2012-10-17",
  "Statement": [
    {"Effect": "Allow", "Action": "iot:Connect",
     "Resource": "arn:aws:iot:$REGION:$ACCOUNT:client/$THING"},
    {"Effect": "Allow", "Action": "iot:Publish",
     "Resource": ["arn:aws:iot:$REGION:$ACCOUNT:topic/traffic/pub",
                  "arn:aws:iot:$REGION:$ACCOUNT:topic/traffic/ack"]},
    {"Effect": "Allow", "Action": "iot:Subscribe",
     "Resource": "arn:aws:iot:$REGION:$ACCOUNT:topicfilter/traffic/sub"},
    {"Effect": "Allow", "Action": "iot:Receive",
     "Resource": "arn:aws:iot:$REGION:$ACCOUNT:topic/traffic/sub"}
  ]
}
EOF
)" >/dev/null
  echo "created policy TrafficNodePolicy"
fi

aws iot describe-thing --thing-name "$THING" >/dev/null 2>&1 || {
  aws iot create-thing --thing-name "$THING" >/dev/null
  echo "created thing $THING"
}

if [[ ! -f "$OUT/cert_arn.txt" ]]; then
  aws iot create-keys-and-certificate --set-as-active \
    --certificate-pem-outfile "$OUT/device.pem.crt" \
    --private-key-outfile "$OUT/private.pem.key" \
    --query certificateArn --output text > "$OUT/cert_arn.txt"
  echo "created device certificate"
fi
CERT_ARN=$(cat "$OUT/cert_arn.txt")
aws iot attach-policy --policy-name TrafficNodePolicy --target "$CERT_ARN"
aws iot attach-thing-principal --thing-name "$THING" --principal "$CERT_ARN"
curl -sS -o "$OUT/AmazonRootCA1.pem" https://www.amazontrust.com/repository/AmazonRootCA1.pem
ENDPOINT=$(aws iot describe-endpoint --endpoint-type iot:Data-ATS --query endpointAddress --output text)
echo "endpoint: $ENDPOINT"

# ---------------------------------------------------------------------
step "2/9 DynamoDB tables"
for T in TrafficReadings TrafficPredictions TrafficAlerts; do
  if ! aws dynamodb describe-table --table-name "$T" >/dev/null 2>&1; then
    aws dynamodb create-table --table-name "$T" \
      --attribute-definitions AttributeName=device,AttributeType=S AttributeName=ts,AttributeType=N \
      --key-schema AttributeName=device,KeyType=HASH AttributeName=ts,KeyType=RANGE \
      --billing-mode PAY_PER_REQUEST >/dev/null
    echo "created table $T"
  fi
done
for T in TrafficReadings TrafficPredictions TrafficAlerts; do
  aws dynamodb wait table-exists --table-name "$T"
done

# ---------------------------------------------------------------------
step "3/9 SNS email alerts"
TOPIC_ARN=$(aws sns create-topic --name TrafficAlerts --query TopicArn --output text)
EXISTING=$(aws sns list-subscriptions-by-topic --topic-arn "$TOPIC_ARN" \
  --query "Subscriptions[?Endpoint=='$EMAIL'].SubscriptionArn" --output text)
if [[ -z "$EXISTING" ]]; then
  aws sns subscribe --topic-arn "$TOPIC_ARN" --protocol email --notification-endpoint "$EMAIL" >/dev/null
  echo ">>> An email was sent to $EMAIL - open it and click 'Confirm subscription'"
fi

# ---------------------------------------------------------------------
step "4/9 S3 buckets (data + dashboard website)"
for B in "$DATA_BUCKET" "$WEB_BUCKET"; do
  if ! aws s3api head-bucket --bucket "$B" >/dev/null 2>&1; then
    aws s3api create-bucket --bucket "$B" --create-bucket-configuration LocationConstraint=$REGION >/dev/null
    echo "created bucket $B"
  fi
done
aws s3api put-public-access-block --bucket "$WEB_BUCKET" --public-access-block-configuration \
  BlockPublicAcls=true,IgnorePublicAcls=true,BlockPublicPolicy=false,RestrictPublicBuckets=false
aws s3api put-bucket-website --bucket "$WEB_BUCKET" \
  --website-configuration '{"IndexDocument":{"Suffix":"index.html"}}'
if ! aws s3api put-bucket-policy --bucket "$WEB_BUCKET" --policy "{
  \"Version\": \"2012-10-17\",
  \"Statement\": [{\"Effect\": \"Allow\", \"Principal\": \"*\", \"Action\": \"s3:GetObject\",
                  \"Resource\": \"arn:aws:s3:::$WEB_BUCKET/*\"}]}"; then
  echo "!!! Could not make the dashboard public. In S3 > 'Block Public Access settings for this account',"
  echo "!!! untick 'Block all public access', save, then run this script again."
  exit 1
fi

# ---------------------------------------------------------------------
step "5/9 IAM roles"
NEW_ROLE=0
make_role() {  # name, service
  if ! aws iam get-role --role-name "$1" >/dev/null 2>&1; then
    aws iam create-role --role-name "$1" --assume-role-policy-document \
      "{\"Version\":\"2012-10-17\",\"Statement\":[{\"Effect\":\"Allow\",\"Principal\":{\"Service\":\"$2\"},\"Action\":\"sts:AssumeRole\"}]}" >/dev/null
    echo "created role $1"
    NEW_ROLE=1
  fi
}
BASIC=arn:aws:iam::aws:policy/service-role/AWSLambdaBasicExecutionRole

make_role traffic-infer-role lambda.amazonaws.com
aws iam attach-role-policy --role-name traffic-infer-role --policy-arn $BASIC
aws iam put-role-policy --role-name traffic-infer-role --policy-name traffic-infer-access --policy-document "{
  \"Version\": \"2012-10-17\", \"Statement\": [
    {\"Effect\": \"Allow\", \"Action\": [\"dynamodb:Query\", \"dynamodb:PutItem\"],
     \"Resource\": [\"$TABLE_ARN/TrafficReadings\", \"$TABLE_ARN/TrafficPredictions\", \"$TABLE_ARN/TrafficAlerts\"]},
    {\"Effect\": \"Allow\", \"Action\": \"s3:GetObject\", \"Resource\": \"arn:aws:s3:::$DATA_BUCKET/models/*\"},
    {\"Effect\": \"Allow\", \"Action\": \"s3:ListBucket\", \"Resource\": \"arn:aws:s3:::$DATA_BUCKET\"},
    {\"Effect\": \"Allow\", \"Action\": \"sns:Publish\", \"Resource\": \"$TOPIC_ARN\"}]}"

make_role traffic-api-role lambda.amazonaws.com
aws iam attach-role-policy --role-name traffic-api-role --policy-arn $BASIC
aws iam put-role-policy --role-name traffic-api-role --policy-name traffic-api-access --policy-document "{
  \"Version\": \"2012-10-17\", \"Statement\": [
    {\"Effect\": \"Allow\", \"Action\": [\"dynamodb:Query\", \"dynamodb:PutItem\"],
     \"Resource\": [\"$TABLE_ARN/TrafficReadings\", \"$TABLE_ARN/TrafficPredictions\", \"$TABLE_ARN/TrafficAlerts\"]},
    {\"Effect\": \"Allow\", \"Action\": \"iot:Publish\", \"Resource\": \"arn:aws:iot:$REGION:$ACCOUNT:topic/traffic/sub\"},
    {\"Effect\": \"Allow\", \"Action\": \"sns:Publish\", \"Resource\": \"$TOPIC_ARN\"}]}"

make_role traffic-export-role lambda.amazonaws.com
aws iam attach-role-policy --role-name traffic-export-role --policy-arn $BASIC
aws iam put-role-policy --role-name traffic-export-role --policy-name traffic-export-access --policy-document "{
  \"Version\": \"2012-10-17\", \"Statement\": [
    {\"Effect\": \"Allow\", \"Action\": \"dynamodb:Query\", \"Resource\": \"$TABLE_ARN/TrafficReadings\"},
    {\"Effect\": \"Allow\", \"Action\": \"s3:PutObject\", \"Resource\": \"arn:aws:s3:::$DATA_BUCKET/training-data/*\"}]}"

make_role traffic-iot-rule-role iot.amazonaws.com
aws iam put-role-policy --role-name traffic-iot-rule-role --policy-name traffic-iot-rule-access --policy-document "{
  \"Version\": \"2012-10-17\", \"Statement\": [
    {\"Effect\": \"Allow\", \"Action\": \"dynamodb:PutItem\", \"Resource\": \"$TABLE_ARN/TrafficReadings\"}]}"

if [[ $NEW_ROLE == 1 ]]; then
  echo "waiting 15 s for new roles to become active..."
  sleep 15
fi

# ---------------------------------------------------------------------
step "6/9 Lambda functions"
write_env() {  # file, then KEY=VALUE pairs
  local file=$1; shift
  local body="" sep=""
  for kv in "$@"; do body+="$sep\"${kv%%=*}\":\"${kv#*=}\""; sep=","; done
  echo "{\"Variables\":{$body}}" > "$file"
}

deploy_lambda() {  # name, source folder, role, timeout, env file
  local name=$1 src=$2 role=$3 timeout=$4 env=$5
  local zipfile="$OUT/$name.zip"
  rm -f "$zipfile"
  (cd "$src" && python3 -m zipfile -c "$zipfile" *.py)
  local role_arn="arn:aws:iam::$ACCOUNT:role/$role"

  if aws lambda get-function --function-name "$name" >/dev/null 2>&1; then
    aws lambda update-function-code --function-name "$name" --zip-file "fileb://$zipfile" >/dev/null
    aws lambda wait function-updated --function-name "$name"
    aws lambda update-function-configuration --function-name "$name" --role "$role_arn" \
      --timeout "$timeout" --environment "file://$env" >/dev/null
    aws lambda wait function-updated --function-name "$name"
    echo "updated $name"
  else
    for attempt in 1 2 3 4 5 6; do
      if aws lambda create-function --function-name "$name" --runtime python3.12 \
           --handler lambda_function.lambda_handler --role "$role_arn" --timeout "$timeout" \
           --memory-size 256 --zip-file "fileb://$zipfile" --environment "file://$env" >/dev/null 2>"$OUT/err.txt"; then
        break
      fi
      if [[ $attempt == 6 ]]; then cat "$OUT/err.txt"; exit 1; fi
      echo "role not ready yet, retrying in 10 s..."
      sleep 10
    done
    aws lambda wait function-active --function-name "$name"
    echo "created $name"
  fi
}

write_env "$OUT/env_infer.json" "MODEL_BUCKET=$DATA_BUCKET" "SNS_TOPIC_ARN=$TOPIC_ARN" "DASHBOARD_URL=$WEB_URL"
write_env "$OUT/env_api.json" "IOT_DATA_ENDPOINT=$ENDPOINT" "CONTROL_PIN=$PIN" "SNS_TOPIC_ARN=$TOPIC_ARN"
write_env "$OUT/env_export.json" "DATA_BUCKET=$DATA_BUCKET"

deploy_lambda traffic_infer  "$DIR/cloud/lambda_infer"  traffic-infer-role  15 "$OUT/env_infer.json"
deploy_lambda traffic_api    "$DIR/cloud/lambda_api"    traffic-api-role    10 "$OUT/env_api.json"
deploy_lambda traffic_export "$DIR/cloud/lambda_export" traffic-export-role 60 "$OUT/env_export.json"

INFER_ARN="arn:aws:lambda:$REGION:$ACCOUNT:function:traffic_infer"
API_LAMBDA_ARN="arn:aws:lambda:$REGION:$ACCOUNT:function:traffic_api"

# ---------------------------------------------------------------------
step "7/9 IoT rule: traffic/pub -> DynamoDB + traffic_infer"
RULE_PAYLOAD="{
  \"sql\": \"SELECT *, timestamp() AS ts FROM 'traffic/pub'\",
  \"awsIotSqlVersion\": \"2016-03-23\",
  \"ruleDisabled\": false,
  \"actions\": [
    {\"dynamoDBv2\": {\"roleArn\": \"arn:aws:iam::$ACCOUNT:role/traffic-iot-rule-role\",
                     \"putItem\": {\"tableName\": \"TrafficReadings\"}}},
    {\"lambda\": {\"functionArn\": \"$INFER_ARN\"}}
  ]}"
for attempt in 1 2 3 4 5 6; do
  if aws iot get-topic-rule --rule-name TrafficIngestRule >/dev/null 2>&1; then
    aws iot replace-topic-rule --rule-name TrafficIngestRule --topic-rule-payload "$RULE_PAYLOAD" 2>"$OUT/err.txt" && break
  else
    aws iot create-topic-rule --rule-name TrafficIngestRule --topic-rule-payload "$RULE_PAYLOAD" 2>"$OUT/err.txt" && break
  fi
  if [[ $attempt == 6 ]]; then cat "$OUT/err.txt"; exit 1; fi
  echo "rule role not ready yet, retrying in 10 s..."
  sleep 10
done
aws lambda add-permission --function-name traffic_infer --statement-id iot-rule-invoke \
  --action lambda:InvokeFunction --principal iot.amazonaws.com \
  --source-arn "arn:aws:iot:$REGION:$ACCOUNT:rule/TrafficIngestRule" --source-account "$ACCOUNT" \
  >/dev/null 2>&1 || true
echo "rule TrafficIngestRule ready"

# ---------------------------------------------------------------------
step "8/9 API Gateway (HTTP API) for the dashboard"
API_ID=$(aws apigatewayv2 get-apis --query "Items[?Name=='TrafficAPI'].ApiId | [0]" --output text)
if [[ -z "$API_ID" || "$API_ID" == "None" ]]; then
  API_ID=$(aws apigatewayv2 create-api --name TrafficAPI --protocol-type HTTP --target "$API_LAMBDA_ARN" \
    --cors-configuration '{"AllowOrigins":["*"],"AllowMethods":["GET","POST","OPTIONS"],"AllowHeaders":["content-type"]}' \
    --query ApiId --output text)
  echo "created API $API_ID"
fi
aws lambda add-permission --function-name traffic_api --statement-id apigw-invoke \
  --action lambda:InvokeFunction --principal apigateway.amazonaws.com \
  --source-arn "arn:aws:execute-api:$REGION:$ACCOUNT:$API_ID/*" >/dev/null 2>&1 || true
API_URL=$(aws apigatewayv2 get-api --api-id "$API_ID" --query ApiEndpoint --output text)
echo "API: $API_URL"

# ---------------------------------------------------------------------
step "9/9 Dashboard website + NodeMCU secrets.h"
sed "s#https://YOUR-API-ID.execute-api.ap-south-1.amazonaws.com#$API_URL#" \
  "$DIR/dashboard/index.html" > "$OUT/index.html"
aws s3 cp "$OUT/index.html" "s3://$WEB_BUCKET/index.html" \
  --content-type "text/html; charset=utf-8" --cache-control "no-cache" >/dev/null

python3 - "$OUT" "$ENDPOINT" "$THING" <<'PY'
import sys
out, endpoint, thing = sys.argv[1:4]
read = lambda name: open(f"{out}/{name}").read().strip()
text = f'''// Generated by setup.sh - DO NOT share publicly (contains the device private key).
// Only WiFi name/password need to be filled in by hand.
#pragma once

const char WIFI_SSID[]     = "YOUR_WIFI_NAME";      // 2.4 GHz only
const char WIFI_PASSWORD[] = "YOUR_WIFI_PASSWORD";

const char AWS_IOT_ENDPOINT[] = "{endpoint}";
const char DEVICE_ID[] = "{thing}";

static const char AWS_CERT_CA[] PROGMEM = R"EOF(
{read("AmazonRootCA1.pem")}
)EOF";

static const char AWS_CERT_CRT[] PROGMEM = R"EOF(
{read("device.pem.crt")}
)EOF";

static const char AWS_CERT_PRIVATE[] PROGMEM = R"EOF(
{read("private.pem.key")}
)EOF";
'''
open(f"{out}/secrets.h", "w").write(text)
PY

cat > "$OUT/summary.txt" <<EOF
CS4101 Smart Traffic - AWS resources (region $REGION, account $ACCOUNT)
Dashboard URL : $WEB_URL
API URL       : $API_URL
IoT endpoint  : $ENDPOINT
Thing / topic : $THING  |  traffic/pub (data)  traffic/sub (commands)  traffic/ack
Tables        : TrafficReadings, TrafficPredictions, TrafficAlerts
Buckets       : $DATA_BUCKET (data + model), $WEB_BUCKET (dashboard)
SNS topic     : $TOPIC_ARN
Lambdas       : traffic_infer, traffic_api, traffic_export
IoT rule      : TrafficIngestRule
EOF

echo
echo "================ DONE ================"
cat "$OUT/summary.txt"
echo
echo "Next:"
echo " 1. Confirm the SNS email (check spam too)."
echo " 2. Test without hardware:  bash fake_device.sh"
echo " 3. Open the dashboard:     $WEB_URL"
echo " 4. Download secrets.h: Actions > Download file > $OUT/secrets.h"
