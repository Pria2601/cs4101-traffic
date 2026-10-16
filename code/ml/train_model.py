"""
Train the congestion forecasting model on OUR collected sensor data.

Usage (from the code/ml folder):
    pip install pandas scikit-learn matplotlib
    python train_model.py --csv data/traffic_readings.csv

Input : CSV exported by the traffic_export Lambda (download it from S3)
Output: output/congestion_model.json  -> upload to S3 models/congestion_model.json
        output/report.txt             -> dataset details + model results for the report
        output/*.png                  -> graphs for the report
"""
import argparse
import json
import os
import sys
from datetime import datetime

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd
from sklearn.ensemble import RandomForestClassifier
from sklearn.metrics import accuracy_score, confusion_matrix, f1_score
from sklearn.tree import DecisionTreeClassifier

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "cloud", "lambda_infer"))
import traffic_ml as ml  # noqa: E402  (same feature code as the live Lambda)

SESSION_GAP_MS = 15000  # a gap longer than this starts a new collection session


def load_readings(path):
    df = pd.read_csv(path)
    df = df.sort_values("ts").reset_index(drop=True)
    num_cols = [c for c in df.columns if c not in ("device", "mode", "phase", "rsn", "lights")]
    df[num_cols] = df[num_cols].apply(pd.to_numeric, errors="coerce").fillna(0)
    new_session = (df["ts"].diff() > SESSION_GAP_MS) | (df["seq"].diff() < 0)
    df["session"] = new_session.cumsum()
    return df


def build_samples(df):
    X, y, base, ts = [], [], [], []
    for _, s in df.groupby("session"):
        rows = s.to_dict("records")
        for t in range(ml.HISTORY, len(rows) - ml.HORIZON):
            window = rows[t - ml.HISTORY: t + 1]
            future = rows[t + ml.HORIZON]
            for lane in ml.LANES:
                x = ml.lane_features(window, lane)
                X.append(x)
                y.append(ml.level_of(ml.num(future, f"q{lane}")))
                base.append(ml.level_of(x[0]))  # "nothing changes" baseline
                ts.append(rows[t]["ts"])
    return np.array(X), np.array(y), np.array(base), np.array(ts)


def tree_to_json(tree, n_features):
    t = tree.tree_
    return {
        "left": t.children_left.tolist(),
        "right": t.children_right.tolist(),
        "feature": [int(f) if f >= 0 else 0 for f in t.feature],
        "threshold": t.threshold.tolist(),
        "value": [v[0].tolist() for v in t.value],
    }


def export_model(name, clf, n_features):
    trees = clf.estimators_ if hasattr(clf, "estimators_") else [clf]
    return {
        "name": name,
        "trained_at": datetime.now().isoformat(timespec="seconds"),
        "features": ml.FEATURES,
        "levels": ml.LEVELS,
        "classes": [int(c) for c in clf.classes_],
        "history": ml.HISTORY,
        "horizon": ml.HORIZON,
        "trees": [tree_to_json(t, n_features) for t in trees],
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--csv", default="data/traffic_readings.csv")
    ap.add_argument("--out", default="output")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    report = []

    def say(text=""):
        print(text)
        report.append(text)

    # ---------------- Dataset ----------------
    df = load_readings(args.csv)
    X, y, base, ts = build_samples(df)
    if len(y) < 100:
        sys.exit(f"Only {len(y)} samples - collect more data first (aim for 1000+ readings).")

    start = datetime.fromtimestamp(df["ts"].min() / 1000)
    end = datetime.fromtimestamp(df["ts"].max() / 1000)
    say("=== DATASET (collected from our IR sensors via AWS IoT -> DynamoDB -> S3) ===")
    say(f"Readings (rows)         : {len(df)}  (one every ~3 s)")
    say(f"Collection sessions     : {df['session'].nunique()}")
    say(f"Time span               : {start:%d %b %Y %H:%M} to {end:%d %b %Y %H:%M}")
    for sid, s in df.groupby("session"):
        mins = (s["ts"].max() - s["ts"].min()) / 60000
        say(f"  session {sid + 1}: {len(s)} readings, {mins:.1f} min")
    for lane in ml.LANES:
        say(f"Vehicles detected lane {lane}: {int(df[f'a{lane}'].sum())}")
    say(f"Columns per reading     : {len(df.columns) - 1} (queue, wait, arrivals, score, fault per lane + signal state)")
    say(f"Training samples        : {len(y)} (4 lanes x usable readings)")
    say(f"Features                : {', '.join(ml.FEATURES)}")
    say(f"Label                   : congestion level {ml.HORIZON * 3} s later "
        f"(LOW <= {ml.LOW_MAX}, MEDIUM <= {ml.MED_MAX}, HIGH > {ml.MED_MAX} vehicles)")
    for i, name in enumerate(ml.LEVELS):
        say(f"  {name:6s}: {int((y == i).sum())} samples ({(y == i).mean():.0%})")

    # ---------------- Split by time (no peeking at the future) ----------------
    order = np.argsort(ts, kind="stable")
    X, y, base = X[order], y[order], base[order]
    cut = int(len(y) * 0.75)
    X_tr, X_te, y_tr, y_te, base_te = X[:cut], X[cut:], y[:cut], y[cut:], base[cut:]
    say(f"\nTrain / test split      : {len(y_tr)} / {len(y_te)} (first 75% of time / last 25%)")

    # ---------------- Models ----------------
    models = {
        "DecisionTree": DecisionTreeClassifier(max_depth=5, min_samples_leaf=5,
                                               class_weight="balanced", random_state=42),
        "RandomForest": RandomForestClassifier(n_estimators=50, max_depth=6, min_samples_leaf=5,
                                               class_weight="balanced", random_state=42),
    }
    say("\n=== RESULTS ON TEST DATA ===")
    results = {"Baseline (no change)": (accuracy_score(y_te, base_te),
                                        f1_score(y_te, base_te, average="macro"))}
    preds = {}
    for name, clf in models.items():
        clf.fit(X_tr, y_tr)
        p = clf.predict(X_te)
        preds[name] = p
        results[name] = (accuracy_score(y_te, p), f1_score(y_te, p, average="macro"))
    for name, (acc, f1) in results.items():
        say(f"{name:22s} accuracy {acc:.3f}   macro-F1 {f1:.3f}")

    best = max(models, key=lambda n: results[n][1])
    clf = models[best]
    say(f"\nSelected model: {best} (highest macro-F1)")

    # Refit on all data for deployment, then check the JSON export gives identical answers
    clf.fit(X, y)
    model_json = export_model(best, clf, X.shape[1])
    exported = np.array([ml.predict(model_json, list(x))[0] for x in X_te])
    say(f"Export check (JSON vs sklearn): {(exported == clf.predict(X_te)).mean():.1%} match")
    with open(os.path.join(args.out, "congestion_model.json"), "w") as f:
        json.dump(model_json, f)

    # ---------------- Graphs ----------------
    times = pd.to_datetime(df["ts"], unit="ms")
    fig, ax = plt.subplots(figsize=(10, 4))
    for lane in ml.LANES:
        ax.plot(times, df[f"q{lane}"], label=f"Lane {lane}", linewidth=1)
    ax.set(title="Queue length per lane over time", xlabel="Time", ylabel="Vehicles waiting")
    ax.legend()
    fig.tight_layout(); fig.savefig(os.path.join(args.out, "eda_queue_over_time.png"), dpi=150)

    fig, axes = plt.subplots(1, 2, figsize=(10, 4))
    lanes = [f"Lane {l}" for l in ml.LANES]
    axes[0].bar(lanes, [df[f"a{l}"].sum() for l in ml.LANES])
    axes[0].set(title="Total vehicles detected", ylabel="Vehicles")
    axes[1].bar(lanes, [df[f"w{l}"].mean() for l in ml.LANES])
    axes[1].set(title="Average waiting time", ylabel="Seconds")
    fig.tight_layout(); fig.savefig(os.path.join(args.out, "eda_lane_comparison.png"), dpi=150)

    fig, ax = plt.subplots(figsize=(5, 4))
    ax.bar(ml.LEVELS, [(y == i).sum() for i in range(3)])
    ax.set(title="Label distribution", ylabel="Samples")
    fig.tight_layout(); fig.savefig(os.path.join(args.out, "class_distribution.png"), dpi=150)

    cm = confusion_matrix(y_te, preds[best], labels=[0, 1, 2])
    fig, ax = plt.subplots(figsize=(5, 4))
    ax.imshow(cm, cmap="Blues")
    for i in range(3):
        for j in range(3):
            ax.text(j, i, cm[i, j], ha="center", va="center")
    ax.set(xticks=range(3), yticks=range(3), xticklabels=ml.LEVELS, yticklabels=ml.LEVELS,
           xlabel="Predicted", ylabel="Actual", title=f"Confusion matrix ({best})")
    fig.tight_layout(); fig.savefig(os.path.join(args.out, "confusion_matrix.png"), dpi=150)

    fig, ax = plt.subplots(figsize=(7, 4))
    ax.barh(ml.FEATURES, clf.feature_importances_)
    ax.set(title=f"Feature importance ({best})", xlabel="Importance")
    fig.tight_layout(); fig.savefig(os.path.join(args.out, "feature_importance.png"), dpi=150)

    fig, ax = plt.subplots(figsize=(7, 4))
    names = list(results)
    ax.bar(names, [results[n][1] for n in names])
    ax.set(title="Model comparison (macro-F1, test data)", ylim=(0, 1))
    fig.tight_layout(); fig.savefig(os.path.join(args.out, "model_comparison.png"), dpi=150)
    plt.close("all")

    with open(os.path.join(args.out, "report.txt"), "w") as f:
        f.write("\n".join(report) + "\n")
    print(f"\nSaved model, report.txt and graphs in '{args.out}/'")
    print("Next: upload output/congestion_model.json to S3 at models/congestion_model.json")


if __name__ == "__main__":
    main()
