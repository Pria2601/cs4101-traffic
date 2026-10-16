#!/usr/bin/env bash
# =====================================================================
# Pretend to be the NodeMCU (for testing without hardware).
# Publishes fake readings to traffic/pub every 3 s.
#   bash fake_device.sh        -> 40 readings (~2 min)
#   bash fake_device.sh 100    -> 100 readings
# Lane 4 gets busy halfway (congestion email); the last 3 readings
# report a Lane 2 sensor fault (fault email).
# NOTE: fake data is for testing only - do NOT use it as the training dataset.
# =====================================================================
set -euo pipefail
export AWS_DEFAULT_REGION=ap-south-1
export AWS_PAGER=""
N="${1:-40}"

python3 - "$N" > /tmp/fake_readings.jsonl <<'PY'
import json, random, sys
n = int(sys.argv[1])
random.seed(1)
q = [1, 0, 2, 0]
w = [0, 0, 0, 0]
total = [0, 0, 0, 0]
green = 1
for i in range(n):
    if i % 4 == 0:
        green = max(range(4), key=lambda l: 2 * q[l] + 0.5 * w[l]) + 1
    arrivals = [random.choice([0, 0, 1]) for _ in range(4)]
    if n // 3 <= i < 2 * n // 3:
        arrivals[3] += 2  # lane 4 rush
    msg = {"device": "traffic_node_1", "seq": i, "up": 3 * i, "mode": "AUTO", "ml": 0,
           "green": green, "phase": "G", "rem": 5, "rsn": "score",
           "lights": "".join("G" if l + 1 == green else "R" for l in range(4))}
    for l in range(4):
        if l + 1 == green:
            q[l] = max(0, q[l] - 2)
            w[l] = 0
        else:
            q[l] = min(20, q[l] + arrivals[l])
            w[l] = w[l] + 3 if q[l] else 0
        total[l] += arrivals[l]
        msg.update({f"q{l+1}": q[l], f"w{l+1}": w[l], f"a{l+1}": arrivals[l], f"t{l+1}": total[l],
                    f"s{l+1}": round(2 * q[l] + 0.5 * w[l], 1),
                    f"f{l+1}": 1 if (l == 1 and i >= n - 3) else 0})
    msg.update({"wq": 2.0, "ww": 0.5, "ming": 5, "maxg": 20, "pv": 2, "mw": 40})
    print(json.dumps(msg))
PY

i=0
while IFS= read -r line; do
  i=$((i + 1))
  aws iot-data publish --topic traffic/pub --qos 1 \
    --cli-binary-format raw-in-base64-out --payload "$line"
  echo "sent $i/$N  $(echo "$line" | python3 -c 'import json,sys; m=json.load(sys.stdin); print("green L%d  queues %s" % (m["green"], [m["q%d" % l] for l in range(1, 5)]))')"
  sleep 3
done < /tmp/fake_readings.jsonl

echo
echo "Done. Check: dashboard, DynamoDB tables, your email."
