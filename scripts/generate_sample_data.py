#!/usr/bin/env python3
"""Generate a synthetic historical order-flow CSV for LightningLOB's replay demo.

The output is deterministic (fixed RNG seed) so results are reproducible.
Prices follow a simple random walk in integer ticks around a starting mid,
which is enough to exercise realistic-looking book depth and matching
without needing real market data.

Usage:
    python3 scripts/generate_sample_data.py [rows] [output_path] [seed]

Defaults: 5000 rows -> data/sample_orders.csv, seed 42.
"""
import csv
import random
import sys


def generate(num_rows: int, seed: int):
    rng = random.Random(seed)
    rows = []
    mid_price = 10_000  # integer ticks, e.g. 100.00 at 0.01 tick size
    timestamp_ns = 1_700_000_000_000_000_000  # arbitrary fixed epoch-ish start

    for order_id in range(1, num_rows + 1):
        # Mid-price random walk, gently mean-reverting so it doesn't drift
        # off to an unrealistic extreme over thousands of rows.
        mid_price += rng.randint(-3, 3)
        mid_price += (10_000 - mid_price) // 200
        mid_price = max(mid_price, 100)

        side = "BUY" if rng.random() < 0.5 else "SELL"

        # Mostly resting limit orders around the touch, a modest slice of
        # marketable limits/market orders to generate trades, and a small
        # fraction of IOC.
        roll = rng.random()
        if roll < 0.08:
            order_type = "MARKET"
            price = 0
            tif = "IOC"
        else:
            order_type = "LIMIT"
            offset = rng.randint(-15, 15)
            price = max(mid_price + offset, 1)
            tif = "IOC" if rng.random() < 0.05 else "GTC"

        quantity = rng.choice([1, 5, 10, 10, 20, 25, 50, 100])
        timestamp_ns += rng.randint(500, 50_000)  # ~microsecond-scale inter-arrival

        rows.append([order_id, timestamp_ns, 1, side, order_type, price, quantity, tif])

    return rows


def main():
    num_rows = int(sys.argv[1]) if len(sys.argv) > 1 else 5000
    output_path = sys.argv[2] if len(sys.argv) > 2 else "data/sample_orders.csv"
    seed = int(sys.argv[3]) if len(sys.argv) > 3 else 42

    rows = generate(num_rows, seed)
    with open(output_path, "w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(["order_id", "timestamp_ns", "symbol_id", "side", "type", "price", "quantity", "time_in_force"])
        writer.writerows(rows)

    print(f"Wrote {len(rows)} rows to {output_path}")


if __name__ == "__main__":
    main()
