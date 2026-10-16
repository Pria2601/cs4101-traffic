"""
Shared ML code: used by BOTH the training script (ml/train_model.py)
and the live inference Lambda (traffic_infer). Keep one copy only.

Task: predict each lane's congestion level ~30 s ahead
      (LOW / MEDIUM / HIGH) from the latest few sensor readings.
"""

LANES = [1, 2, 3, 4]
HISTORY = 3        # past readings used for features (3 x 3 s = 9 s)
HORIZON = 10       # predict 10 readings ahead (10 x 3 s = 30 s)
LOW_MAX = 2        # queue 0-2   -> LOW
MED_MAX = 5        # queue 3-5   -> MEDIUM, 6+ -> HIGH
LEVELS = ["LOW", "MEDIUM", "HIGH"]

FEATURES = [
    "queue",          # vehicles waiting now
    "wait_s",         # seconds the first vehicle has waited
    "is_green",       # 1 if this lane is green now
    "arrivals_9s",    # vehicles detected in the last 3 readings
    "queue_change",   # queue now minus queue 3 readings ago
    "others_queue",   # average queue of the other 3 lanes
]


def num(row, key, default=0.0):
    try:
        return float(row.get(key, default))
    except (TypeError, ValueError):
        return default


def level_of(queue):
    if queue <= LOW_MAX:
        return 0
    if queue <= MED_MAX:
        return 1
    return 2


def lane_features(rows, lane):
    """rows: readings oldest -> newest; rows[-1] is 'now'."""
    now = rows[-1]
    recent = rows[-HISTORY:]
    past = rows[-1 - HISTORY] if len(rows) > HISTORY else rows[0]

    queue = num(now, f"q{lane}")
    others = [num(now, f"q{o}") for o in LANES if o != lane]
    is_green = 1.0 if int(num(now, "green")) == lane and now.get("phase") == "G" else 0.0

    return [
        queue,
        num(now, f"w{lane}"),
        is_green,
        sum(num(r, f"a{lane}") for r in recent),
        queue - num(past, f"q{lane}"),
        sum(others) / len(others),
    ]


# ---------------- Model stored as plain JSON (no sklearn needed in Lambda) ----------------

def _tree_proba(tree, x):
    node = 0
    while tree["left"][node] != -1:
        if x[tree["feature"][node]] <= tree["threshold"][node]:
            node = tree["left"][node]
        else:
            node = tree["right"][node]
    counts = tree["value"][node]
    total = sum(counts) or 1.0
    return [c / total for c in counts]


def predict(model, x):
    """Returns (level_index, probabilities). Works for 1 tree or a forest."""
    probs = [0.0] * len(LEVELS)
    for tree in model["trees"]:
        for i, p in enumerate(_tree_proba(tree, x)):
            probs[model["classes"][i]] += p
    n = len(model["trees"])
    probs = [p / n for p in probs]
    best = max(range(len(probs)), key=lambda i: probs[i])
    return best, probs


def predict_rule(x):
    """Fallback before a model is trained: current queue level, full confidence."""
    lvl = level_of(x[0])
    probs = [0.0] * len(LEVELS)
    probs[lvl] = 1.0
    return lvl, probs
