#!/usr/bin/env python3
"""Render one markdown table from a bench manifest.

Server columns are quantiles of the exported Prometheus buckets, subtracted
across the run. Those buckets are coarser than the in-process HDR. Client
columns are copied from the wrk2 log. A missing sample stays blank.
"""

import argparse
import sys
from pathlib import Path

BOUNDS = (
    "0.0005",
    "0.001",
    "0.002",
    "0.005",
    "0.01",
    "0.02",
    "0.05",
    "0.1",
    "0.25",
    "0.5",
    "1",
    "2.5",
    "+Inf",
)
QUANTILES = (0.50, 0.95, 0.99, 0.999)


def parse_buckets(text: str) -> dict[str, dict[str, int]]:
    out: dict[str, dict[str, int]] = {}
    for line in text.splitlines():
        if not line.startswith("kestrel_request_duration_seconds_bucket{"):
            continue
        try:
            labels, raw = line.split("} ", 1)
            result = labels.split('result="', 1)[1].split('"', 1)[0]
            le = labels.split('le="', 1)[1].split('"', 1)[0]
            count = int(raw.strip())
        except (IndexError, ValueError):
            continue
        out.setdefault(result, {})[le] = count
    return out


def parse_counts(text: str) -> dict[str, int]:
    out: dict[str, int] = {}
    prefix = "kestrel_request_duration_seconds_count{"
    for line in text.splitlines():
        if not line.startswith(prefix):
            continue
        try:
            result = line.split('result="', 1)[1].split('"', 1)[0]
            out[result] = int(line.rsplit(" ", 1)[1])
        except (IndexError, ValueError):
            continue
    return out


def combined(buckets: dict[str, dict[str, int]]) -> list[tuple[float, int]]:
    series: list[tuple[float, int]] = []
    carried = {kind: 0 for kind in buckets}
    for le in BOUNDS:
        total = 0
        for kind, hist in buckets.items():
            if le in hist:
                carried[kind] = hist[le]
            total += carried[kind]
        if le == "+Inf":
            series.append((float("inf"), total))
        else:
            series.append((float(le), total))
    return series


def delta_buckets(before: dict[str, dict[str, int]], after: dict[str, dict[str, int]]) -> dict[str, dict[str, int]]:
    kinds = set(before) | set(after)
    out: dict[str, dict[str, int]] = {}
    for kind in kinds:
        out[kind] = {}
        for le in BOUNDS:
            value = after.get(kind, {}).get(le, 0) - before.get(kind, {}).get(le, 0)
            out[kind][le] = value if value > 0 else 0
    return out


def quantile_us(series: list[tuple[float, int]], q: float) -> str:
    if not series or series[-1][1] <= 0 or not 0 < q <= 1:
        return ""
    rank = q * series[-1][1]
    prev_le = 0.0
    prev_count = 0
    for le, count in series:
        if count < rank:
            prev_le = le
            prev_count = count
            continue
        if le == float("inf"):
            return ">2500000"
        span = count - prev_count
        if span <= 0:
            return str(int(round(le * 1_000_000)))
        seconds = prev_le + (le - prev_le) * ((rank - prev_count) / span)
        return str(int(round(seconds * 1_000_000)))
    return ""


def wrk_latency_us(text: str) -> dict[str, str]:
    wanted = {"50.000": "p50", "50": "p50", "99.000": "p99", "99": "p99", "99.900": "p99.9", "99.9": "p99.9"}
    found: dict[str, str] = {}
    for line in text.splitlines():
        parts = line.split()
        if len(parts) != 2 or not parts[0].endswith("%"):
            continue
        label = wanted.get(parts[0][:-1])
        if label is None or label in found:
            continue
        raw = parts[1]
        for suffix, scale in (("us", 1.0), ("ms", 1000.0), ("s", 1_000_000.0)):
            if raw.endswith(suffix):
                try:
                    found[label] = str(int(round(float(raw[: -len(suffix)]) * scale)))
                except ValueError:
                    pass
                break
    return found


def wrk_summary(text: str) -> tuple[str, str]:
    requests = ""
    errors = ""
    for line in text.splitlines():
        if "Requests/sec:" in line:
            requests = line.split(":", 1)[1].strip()
        elif line.startswith("Non-2xx") or line.startswith("Socket errors"):
            errors = line.split(":", 1)[-1].strip()
    return requests, errors


def cell(value: str) -> str:
    return value if value else ""


def render(manifest: Path) -> str:
    header = (
        "| target | rate | payload | hit ratio | requests/s | non-2xx | "
        "client p50 us | client p99 us | client p99.9 us | "
        "server p50 us | server p95 us | server p99 us | server p99.9 us | "
        "hit | fill | kestrel cpu s | upstream cpu s |\n"
        "| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |\n"
    )
    rows = [header]
    for line in manifest.read_text().splitlines():
        if not line or line.startswith("#"):
            continue
        fields = line.split("\t")
        if len(fields) != 10:
            raise SystemExit(f"manifest row needs 10 fields: {line}")
        target, rate, payload, hit, wrk_path, before_path, after_path, cpu_k, cpu_u, note = fields
        wrk = Path(wrk_path).read_text() if wrk_path else ""
        client = wrk_latency_us(wrk)
        requests, errors = wrk_summary(wrk)
        server = {q: "" for q in QUANTILES}
        hit_n = ""
        fill_n = ""
        if before_path and after_path:
            before = Path(before_path).read_text()
            after = Path(after_path).read_text()
            series = combined(delta_buckets(parse_buckets(before), parse_buckets(after)))
            server = {q: quantile_us(series, q) for q in QUANTILES}
            counts_before = parse_counts(before)
            counts_after = parse_counts(after)
            hit_n = str(max(0, counts_after.get("hit", 0) - counts_before.get("hit", 0)))
            fill_n = str(max(0, counts_after.get("fill", 0) - counts_before.get("fill", 0)))
        if note:
            errors = note if not errors else f"{errors}; {note}"
        rows.append(
            "| "
            + " | ".join(
                [
                    cell(target),
                    cell(rate),
                    cell(payload),
                    cell(hit),
                    cell(requests),
                    cell(errors),
                    cell(client.get("p50", "")),
                    cell(client.get("p99", "")),
                    cell(client.get("p99.9", "")),
                    cell(server[0.50]),
                    cell(server[0.95]),
                    cell(server[0.99]),
                    cell(server[0.999]),
                    cell(hit_n),
                    cell(fill_n),
                    cell(cpu_k),
                    cell(cpu_u),
                ]
            )
            + " |\n"
        )
    return "".join(rows)


def self_check() -> int:
    after = "\n".join(
        [
            'kestrel_request_duration_seconds_bucket{result="hit",le="0.001"} 0',
            'kestrel_request_duration_seconds_bucket{result="hit",le="0.002"} 100',
            'kestrel_request_duration_seconds_bucket{result="hit",le="+Inf"} 100',
            'kestrel_request_duration_seconds_count{result="hit"} 100',
            "",
        ]
    )
    series = combined(delta_buckets(parse_buckets(""), parse_buckets(after)))
    got = quantile_us(series, 0.50)
    if got != "1500":
        print(f"self-check p50 {got} != 1500", file=sys.stderr)
        return 1
    wrk = "  50.000%    1.25ms\n  99.000%    8ms\n  99.900%    12.5ms\nRequests/sec:   10.00\n"
    latency = wrk_latency_us(wrk)
    if latency.get("p50") != "1250" or latency.get("p99") != "8000" or latency.get("p99.9") != "12500":
        print(f"self-check wrk parse {latency}", file=sys.stderr)
        return 1
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="render a bench table")
    parser.add_argument("--self-check", action="store_true")
    parser.add_argument("--manifest", type=Path)
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    if args.self_check:
        return self_check()
    if args.manifest is None or args.out is None:
        print("need --manifest and --out", file=sys.stderr)
        return 2
    text = (
        "Server columns are quantiles of the exported histogram, in microseconds, "
        "for this run only. Client columns are wrk2's report. A blank cell was not in the log.\n\n"
        + render(args.manifest)
    )
    args.out.write_text(text)
    sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
