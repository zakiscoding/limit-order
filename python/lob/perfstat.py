"""Summarize `perf stat -x,` output collected by scripts/perf_profile.sh.

The script runs the same engine command at 0 passes (setup only) and at N
passes, several times each, for each event group. This module subtracts the
setup runs from the loop runs and divides by the number of events applied, so
the figures describe the replay loop and not log generation.

    python -m lob.perfstat OUT_DIR

reads OUT_DIR/meta.txt and the stat_*.csv files, writes OUT_DIR/summary.md,
and prints it.
"""

from __future__ import annotations

import re
import statistics
import sys
from pathlib import Path

# Events are measured in separate groups so no group needs more counters than a
# core has, which avoids multiplexing and its scaling error.
EVENT_GROUPS: dict[str, list[str]] = {
    "core": ["cycles", "instructions", "branches", "branch-misses"],
    "cache": ["cache-references", "cache-misses", "L1-dcache-loads", "L1-dcache-load-misses"],
    "llc_tlb": ["LLC-loads", "LLC-load-misses", "dTLB-loads", "dTLB-load-misses"],
    "software": ["task-clock", "page-faults", "context-switches", "cpu-migrations"],
}

# (label, numerator, denominator). Both events come from the same group.
RATIOS: list[tuple[str, str, str]] = [
    ("instructions per cycle", "instructions", "cycles"),
    ("branch miss rate", "branch-misses", "branches"),
    ("cache miss rate (cache-misses / cache-references)", "cache-misses", "cache-references"),
    ("L1 data cache load miss rate", "L1-dcache-load-misses", "L1-dcache-loads"),
    ("LLC load miss rate", "LLC-load-misses", "LLC-loads"),
    ("dTLB load miss rate", "dTLB-load-misses", "dTLB-loads"),
]

_PREFIX = re.compile(r"^(?:cpu_core|cpu_atom|cpu)/")
_MODIFIER = re.compile(r"[/:][ukhGHpP]+$")


def normalize_event(name: str) -> str:
    """cpu_core/cycles/u -> cycles, instructions:u -> instructions."""
    name = name.strip()
    name = _PREFIX.sub("", name).rstrip("/")
    name = _MODIFIER.sub("", name)
    return name.rstrip("/")


def parse_stat_csv(text: str) -> dict[str, float | None]:
    """Event name -> value, or None when perf reported it unsupported or not counted.

    On a hybrid CPU perf prints one line per core type for the same event. The
    numeric lines are summed; the other type is normally "<not counted>".
    """
    values: dict[str, float | None] = {}
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        fields = line.split(",")
        if len(fields) < 3:
            continue
        raw, event = fields[0].strip(), normalize_event(fields[2])
        try:
            value: float | None = float(raw)
        except ValueError:
            value = None  # "<not supported>" or "<not counted>"
        if value is None:
            values.setdefault(event, None)
        else:
            values[event] = (values.get(event) or 0.0) + value
    return values


def _spread(values: list[float]) -> tuple[float, float, float]:
    return statistics.median(values), min(values), max(values)


def aggregate(base: dict[str, list[dict[str, float | None]]], loop: dict[str, list[dict[str, float | None]]]):
    """Per-event loop totals with the setup baseline removed.

    base and loop map a group name to a list of parsed runs. Returns
    (totals, ratios): totals[event] = (median, min, max) of loop - median(base);
    ratios[label] = (median, min, max) of the per-run ratio. Events that are
    missing or unsupported in any run are left out.
    """
    totals: dict[str, tuple[float, float, float]] = {}
    per_run: dict[str, list[float]] = {}
    for group, runs in loop.items():
        base_runs = base.get(group, [])
        for event in EVENT_GROUPS.get(group, []):
            loop_vals = [r.get(event) for r in runs]
            base_vals = [r.get(event) for r in base_runs]
            if not loop_vals or any(v is None for v in loop_vals) or not base_vals or any(v is None for v in base_vals):
                continue
            base_med = statistics.median(base_vals)  # type: ignore[arg-type]
            deltas = [v - base_med for v in loop_vals]  # type: ignore[operator]
            per_run[event] = deltas
            totals[event] = _spread(deltas)

    ratios: dict[str, tuple[float, float, float]] = {}
    for label, num, den in RATIOS:
        if num in per_run and den in per_run:
            pairs = [n / d for n, d in zip(per_run[num], per_run[den]) if d > 0]
            if pairs:
                ratios[label] = _spread(pairs)
    return totals, ratios


def read_meta(path: Path) -> dict[str, str]:
    meta: dict[str, str] = {}
    for line in path.read_text().splitlines():
        if "=" in line:
            key, value = line.split("=", 1)
            meta[key.strip()] = value.strip()
    return meta


def load_runs(out_dir: Path, group: str, passes: int) -> list[dict[str, float | None]]:
    runs = []
    for path in sorted(out_dir.glob(f"stat_{group}_p{passes}_r*.csv")):
        runs.append(parse_stat_csv(path.read_text()))
    return runs


def _num(x: float) -> str:
    return f"{x:,.0f}" if abs(x) >= 1000 else f"{x:.3f}".rstrip("0").rstrip(".")


def render(meta: dict[str, str], totals, ratios, unsupported: list[str], alloc: dict[str, str] | None) -> str:
    events = int(meta["events"])
    passes = int(meta["passes"])
    applied = events * passes
    lines = [
        "# perf summary",
        "",
        f"Seed {meta.get('seed', '?')}, {events:,} events, {passes} pass(es) per run, "
        f"{meta.get('repeats', '?')} repeats per measurement. Counts are the loop run minus the "
        f"setup-only run, divided by {applied:,} applied events. Values are the median of the repeats "
        "with [min..max].",
        "",
    ]
    if totals:
        lines += ["| Event | Per event | Total in loop (median) |", "| --- | ---: | ---: |"]
        for event in [e for g in EVENT_GROUPS.values() for e in g]:
            if event not in totals:
                continue
            med, lo, hi = totals[event]
            scale = 1e6 if event == "task-clock" else 1.0  # task-clock is in msec
            unit = " ns" if event == "task-clock" else ""
            lines.append(
                f"| {event} | {_num(med * scale / applied)}{unit} [{_num(lo * scale / applied)}..{_num(hi * scale / applied)}] "
                f"| {_num(med)} |"
            )
        lines.append("")
    if ratios:
        lines += ["| Ratio | Value |", "| --- | ---: |"]
        for label, _, _ in RATIOS:
            if label in ratios:
                med, lo, hi = ratios[label]
                if "rate" in label:
                    lines.append(f"| {label} | {med * 100:.3f}% [{lo * 100:.3f}%..{hi * 100:.3f}%] |")
                else:
                    lines.append(f"| {label} | {med:.3f} [{lo:.3f}..{hi:.3f}] |")
        lines.append("")
    if unsupported:
        lines += ["Not measured (unsupported or not counted on this machine): " + ", ".join(sorted(unsupported)) + ".", ""]
    if not totals:
        lines += ["No hardware or software counter data was collected.", ""]
    if alloc:
        lines += [
            "Allocation behavior (global operator new/delete, apply calls only):",
            "",
            "| Measure | Value |",
            "| --- | ---: |",
        ]
        n_passes = max(int(alloc.get("passes", "1")), 1)
        for key, label in [
            ("new_calls", "operator new calls"),
            ("delete_calls", "operator delete calls"),
            ("bytes_allocated", "bytes allocated (usable size)"),
            ("peak_live_growth_bytes", "peak live bytes above start (last pass)"),
            ("live_growth_bytes", "live bytes above start at end of loop (last pass)"),
            ("minor_faults", "minor page faults in the loop"),
            ("major_faults", "major page faults in the loop"),
            ("max_rss_kib", "peak RSS (KiB, whole process)"),
        ]:
            if key in alloc:
                lines.append(f"| {label} | {int(alloc[key]):,} |")
        if n_passes == 1 and alloc.get("events"):
            e = int(alloc["events"])
            lines.append(f"| operator new calls per event | {int(alloc['new_calls']) / e:.4f} |")
        lines.append("")
    return "\n".join(lines)


def parse_apply_loop_line(text: str) -> dict[str, str] | None:
    for line in text.splitlines():
        if line.startswith("APPLY_LOOP "):
            return dict(tok.split("=", 1) for tok in line.split()[1:] if "=" in tok)
    return None


def summarize_dir(out_dir: Path) -> str:
    meta = read_meta(out_dir / "meta.txt")
    passes = int(meta["passes"])
    groups = [g for g in meta.get("groups", "").split() if g in EVENT_GROUPS]
    base = {g: load_runs(out_dir, g, 0) for g in groups}
    loop = {g: load_runs(out_dir, g, passes) for g in groups}
    totals, ratios = aggregate(base, loop)
    wanted = {e for g in groups for e in EVENT_GROUPS[g]}
    unsupported = sorted(wanted - set(totals))
    alloc = None
    alloc_path = out_dir / "alloc.txt"
    if alloc_path.exists():
        alloc = parse_apply_loop_line(alloc_path.read_text())
    return render(meta, totals, ratios, unsupported, alloc)


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print("usage: python -m lob.perfstat OUT_DIR", file=sys.stderr)
        return 2
    out_dir = Path(argv[1])
    text = summarize_dir(out_dir)
    (out_dir / "summary.md").write_text(text + "\n")
    print(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
