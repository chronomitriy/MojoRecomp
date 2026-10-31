#!/usr/bin/env python3
"""Summarize where a MojoRecomp COT runtime.log spends its frame time.

Works on logs from the released 0.2.0 runtime and from hfr-unlimited builds.
It reads only the runtime's own diagnostic lines; it never opens game data.

  python tools/summarize-runtime-log.py "%LOCALAPPDATA%\\MojoRecomp\\logs\\cot\\runtime.log"
"""

from __future__ import annotations

import re
import statistics
import sys
from collections import Counter
from pathlib import Path

PERF_RE = re.compile(
    r"Vulkan perf: (?P<fps>[\d.]+) fps (?P<frame>[\d.]+) ms/frame "
    r"\(guest\+PM4 (?P<upstream>[\d.]+) ms, present (?P<present>[\d.]+) ms, "
    r"fence (?P<fence>[\d.]+)")
CPU_RE = re.compile(
    r"Vulkan CPU profile: drawTotal (?P<drawTotal>[\d.]+) drawOther (?P<drawOther>[\d.]+) "
    r"attach (?P<attach>[\d.]+) ms/frame edramPipe (?P<edramPipe>[\d.]+) "
    r"edramXfer (?P<edramXfer>[\d.]+) tex (?P<tex>[\d.]+) pipe (?P<pipe>[\d.]+) "
    r"const (?P<const>[\d.]+) vertex (?P<vertex>[\d.]+) resolve (?P<resolve>[\d.]+) "
    r"draws (?P<draws>[\d.]+)/frame resolves (?P<resolves>[\d.]+)/frame")
PROBE_RE = re.compile(
    r"gpu-probe frame=(?P<frame>\d+) .*?backpressure=(?P<backpressure>[\d.]+)ms "
    r"worker=(?P<worker>[\d.]+)ms")
HEALTH_RE = re.compile(r"Vulkan raster health: .*?pipelines=(?P<pipelines>\d+) modules=(?P<modules>\d+)")
PIPELINE_RE = re.compile(r"\[pipeline\] frames=\d+ fps=(?P<fps>[\d.]+) frame=(?P<frame>[\d.]+)ms "
                         r"busiest=(?P<busiest>\w+)\((?P<share>\d+)%\)")
POLICY_RE = re.compile(r"frame policy simulation=(?P<sim>\d+)Hz presentation=(?P<pres>\d+)Hz")
# Phase 4 (docs/HFR-PHASE4.md): effective options, shader replacement, FSR and profilers.
GRAPHICS_RE = re.compile(r"cot-runtime: (?P<text>graphics resolution_scale=.*)$")
PS_REPLACE_RE = re.compile(r"\[ps-replace\] (?P<text>.*)$")
FSR_RE = re.compile(r"(?P<text>Vulkan FSR (active|shader compile failed): .*)$")
GPU_PROFILE_RE = re.compile(r"\[gpu-profile\] frames=\d+ gpu=(?P<gpu>[\d.]+)ms "
                            r"max=(?P<max>[\d.]+)ms \|(?P<categories>[^|]*)\| per frame: "
                            r"(?P<counts>.*?) overflow=(?P<overflow>\d+) malformed=(?P<malformed>\d+)")
GUEST_SUMMARY_RE = re.compile(r"\[guest-profile\] summary scope=(?P<scope>\w+) "
                              r"thread=(?P<thread>\w+) seconds=(?P<seconds>[\d.]+) "
                              r"samples=(?P<samples>\d+) rate=\d+/s (?P<kinds>.*)$")
GUEST_ENTRY_RE = re.compile(r"\[guest-profile\] (?P<what>fn|bucket) scope=(?P<scope>\w+) "
                            r"thread=(?P<thread>\w+) rank=(?P<rank>\d+) pct=(?P<pct>[\d.]+) "
                            r"samples=\d+ name=(?P<name>\S+)")
GUEST_CALLER_RE = re.compile(r"\[guest-profile\] caller scope=(?P<scope>\w+) "
                             r"thread=(?P<thread>\w+) bucket=(?P<bucket>\S+) "
                             r"pct=(?P<pct>[\d.]+) samples=\d+ name=(?P<name>\S+)")
# Per-frame fields of the [pipeline] line (docs/HFR-UNLIMITED.md section 2.7).
PIPELINE_FIELDS = {
    "guest cpu ms": re.compile(r"guest cpu=([\d.]+)ms"),
    "cpWait ms": re.compile(r"cpWait=([\d.]+)ms"),
    "pm4 busy ms": re.compile(r"pm4 busy=([\d.]+)ms"),
    "pm4 sink ms": re.compile(r" sink=([\d.]+)ms"),
    "backpressure ms": re.compile(r"backpressure=([\d.]+)ms"),
    "pm4 stall ms": re.compile(r" stall=([\d.]+)ms"),
    "stalls": re.compile(r" stall=[\d.]+ms/([\d.]+)x"),
    "vblankReleased": re.compile(r"vblankReleased=([\d.]+)x"),
    "vulkan worker ms": re.compile(r"vulkan worker=([\d.]+)ms"),
    "snap hit %": re.compile(r"snap hit=([\d.]+)%"),
    "snap volatile": re.compile(r"volatile=([\d.]+)"),
    "snap cmd": re.compile(r"cmd=([\d.]+)"),
    "snap copied MiB": re.compile(r"copied=([\d.]+)MiB"),
    "snap faults": re.compile(r"faults=([\d.]+)"),
    "snap protects": re.compile(r"protects=([\d.]+)"),
}
# Phase 3 wait breakdown (docs/HFR-PHASE3.md section 2).
PIPELINE_FIELDS.update({
    "polls/frame": re.compile(r" polls=([\d.]+)"),
    "frameLag ms": re.compile(r" lag=([\d.]+)ms/"),
    "idleWait ms": re.compile(r"idleWait=([\d.]+)ms"),
    "ring avg KiB": re.compile(r"ring avg=([\d.]+)KiB"),
    "ring max KiB": re.compile(r"ring avg=[\d.]+KiB max=([\d.]+)KiB"),
    "signals cpTime": re.compile(r"signals cpTime=([\d.]+)"),
    "signals deferred": re.compile(r"signals cpTime=[\d.]+ deferred=([\d.]+)"),
    "inflight": re.compile(r"inflight=([\d.]+)"),
})
# Bracketed groups of name=ms values: cpWait by PM4 activity, stall by reason.
GROUP_RES = {
    "cpWait/pm4": re.compile(r"cpBy\[([^\]]*)\]"),
    "pm4 stall/": re.compile(r"stallBy\[([^\]]*)\]"),
}
TICKS_RE = re.compile(r"gate releases=(?P<releases>\d+) ticks: (?P<ticks>.*)$")
HFR_RE = re.compile(r"\[hfr\] (?P<text>(scheduler floor lifted|present interval|unlocked rate|"
                    r"MojoRecompHfrSchedulerFloor saw).*)$")


def percentile(values: list[float], fraction: float) -> float:
    ordered = sorted(values)
    index = min(len(ordered) - 1, max(0, round(fraction * (len(ordered) - 1))))
    return ordered[index]


def describe(values: list[float]) -> str:
    if not values:
        return "n/a"
    return (f"median {statistics.median(values):6.2f}  p10 {percentile(values, 0.1):6.2f}  "
            f"p90 {percentile(values, 0.9):6.2f}")


def median_of(windows: list[dict[str, float]], name: str) -> float | None:
    values = [w[name] for w in windows if name in w]
    return statistics.median(values) if values else None


def report_window_class(windows: list[dict[str, float]], title: str, keep,
                        group_names: list[str]) -> None:
    """Medians over one class of [pipeline] windows, and which wait dominates."""
    chosen = [w for w in windows if keep(w["fps"])]
    if not chosen:
        return
    print(f"\n  {title}: {len(chosen)} windows, medians per frame")
    for name in ["guest cpu ms", "cpWait ms", "pm4 busy ms", "pm4 stall ms", "vblankReleased",
                 "vulkan worker ms", "backpressure ms", "frameLag ms", "idleWait ms",
                 "ring avg KiB", "inflight", "signals cpTime", "signals deferred",
                 *group_names]:
        value = median_of(chosen, name)
        if value is not None:
            print(f"    {name:<20} {value:7.2f}")
    # Name the largest share of the guest's wait and of the PM4 thread's stall.
    for group in GROUP_RES:
        parts = {name[len(group):-3].strip(): median_of(chosen, name)
                 for name in group_names if name.startswith(group)}
        parts = {k: v for k, v in parts.items() if v is not None}
        if parts and sum(parts.values()) > 0.05:
            top = max(parts, key=parts.get)
            print(f"    => {group.rstrip('/')}: mostly '{top}' ({parts[top]:.2f} of "
                  f"{sum(parts.values()):.2f} ms per frame)")


def pairs(text: str) -> dict[str, float]:
    """`a=1.5 b=2` -> {"a": 1.5, "b": 2.0}."""
    result = {}
    for item in text.split():
        key, _, value = item.partition("=")
        try:
            result[key] = float(value)
        except ValueError:
            pass
    return result


def report_gpu_profile(windows: list[dict]) -> None:
    print(f"\n{len(windows)} '[gpu-profile]' windows (MOJORECOMP_GPU_PROFILE=1), "
          "medians per frame:")
    total = statistics.median(w["gpu"] for w in windows)
    worst = max(w["max"] for w in windows)
    categories = list(dict.fromkeys(k for w in windows for k in w["categories"]))
    medians = {k: statistics.median(w["categories"].get(k, 0.0) for w in windows)
               for k in categories}
    shown = "  ".join(f"{k}={v:.2f}" for k, v in sorted(medians.items(), key=lambda kv: -kv[1])
                      if v >= 0.005)
    print(f"  gpu-profile ms/frame: total={total:.2f} (worst frame {worst:.1f})  {shown}")
    counts = list(dict.fromkeys(k for w in windows for k in w["counts"]))
    print("  gpu-profile per frame: " + "  ".join(
        f"{k}={statistics.median(w['counts'].get(k, 0.0) for w in windows):.1f}"
        for k in counts))
    overflow = sum(w["overflow"] for w in windows)
    malformed = sum(w["malformed"] for w in windows)
    if overflow or malformed:
        print(f"  gpu-profile frames with dropped marks={overflow} unreadable={malformed}")


def report_guest_profile(summaries: list[dict], entries: list[dict],
                         callers: list[dict]) -> None:
    # The whole-session report when the game was closed normally, otherwise the
    # last 15-second window.
    scope = "total" if any(s["scope"] == "total" for s in summaries) else "window"
    for thread in dict.fromkeys(s["thread"] for s in summaries):
        chosen = [s for s in summaries if s["scope"] == scope and s["thread"] == thread]
        if not chosen:
            continue
        summary = chosen[-1]
        # Entries belong to the last report of that scope and thread.
        last = max(i for i, s in enumerate(summaries) if s is summary)
        following = [s["index"] for s in summaries[last + 1:]]
        end = following[0] if following else float("inf")
        mine = [e for e in entries if summary["index"] < e["index"] < end]
        mine_callers = [c for c in callers if summary["index"] < c["index"] < end]
        kinds = "  ".join(f"{k}={v:.1f}%" for k, v in summary["kinds"].items() if v >= 0.05)
        print(f"\nguest-profile {scope} thread={thread}: {summary['seconds']:.0f} s, "
              f"{summary['samples']} samples: {kinds}")
        for e in [e for e in mine if e["what"] == "fn"][:12]:
            print(f"  guest-profile fn     {e['pct']:6.2f}%  {e['name']}")
        for e in [e for e in mine if e["what"] == "bucket"][:8]:
            top = [c for c in mine_callers if c["bucket"] == e["name"]][:2]
            via = ("  from " + ", ".join(f"{c['name']} {c['pct']:.1f}%" for c in top)) if top else ""
            print(f"  guest-profile bucket {e['pct']:6.2f}%  {e['name']}{via}")


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(__doc__.strip())
        return 2
    path = Path(argv[1])
    text = path.read_text(encoding="utf-8", errors="replace").splitlines()

    perf, cpu, probes, pipeline = [], [], [], []
    fields: dict[str, list[float]] = {name: [] for name in PIPELINE_FIELDS}
    windows: list[dict[str, float]] = []
    ticks: Counter = Counter()
    hfr_notes: list[str] = []
    health = None
    policy = None
    phase4_notes: list[str] = []
    gpu_profile: list[dict] = []
    guest_summaries: list[dict] = []
    guest_entries: list[dict] = []
    guest_callers: list[dict] = []
    for index, line in enumerate(text):
        if m := GUEST_ENTRY_RE.search(line):
            guest_entries.append({"index": index, "what": m["what"], "pct": float(m["pct"]),
                                  "name": m["name"]})
        elif m := GUEST_CALLER_RE.search(line):
            guest_callers.append({"index": index, "bucket": m["bucket"], "pct": float(m["pct"]),
                                  "name": m["name"]})
        elif m := GUEST_SUMMARY_RE.search(line):
            guest_summaries.append({"index": index, "scope": m["scope"], "thread": m["thread"],
                                    "seconds": float(m["seconds"]),
                                    "samples": int(m["samples"]), "kinds": pairs(m["kinds"])})
        elif m := GPU_PROFILE_RE.search(line):
            gpu_profile.append({"gpu": float(m["gpu"]), "max": float(m["max"]),
                                "categories": pairs(m["categories"]),
                                "counts": pairs(m["counts"]),
                                "overflow": int(m["overflow"]),
                                "malformed": int(m["malformed"])})
        elif m := (GRAPHICS_RE.search(line) or PS_REPLACE_RE.search(line) or FSR_RE.search(line)):
            if m["text"] not in phase4_notes:
                phase4_notes.append(m["text"])
        elif m := PERF_RE.search(line):
            perf.append({k: float(v) for k, v in m.groupdict().items()})
        elif m := CPU_RE.search(line):
            cpu.append({k: float(v) for k, v in m.groupdict().items()})
        elif m := PROBE_RE.search(line):
            probes.append((int(m["frame"]), float(m["backpressure"]), float(m["worker"])))
        elif m := HEALTH_RE.search(line):
            health = (int(m["pipelines"]), int(m["modules"]))
        elif m := PIPELINE_RE.search(line):
            pipeline.append((float(m["fps"]), float(m["frame"]), m["busiest"], int(m["share"])))
            window = {"fps": float(m["fps"])}
            for name, field_re in PIPELINE_FIELDS.items():
                if f := field_re.search(line):
                    fields[name].append(float(f.group(1)))
                    window[name] = float(f.group(1))
            for group, group_re in GROUP_RES.items():
                if g := group_re.search(line):
                    for item in g.group(1).split():
                        key, _, value = item.partition("=")
                        window[f"{group}{'' if group.endswith('/') else ' '}{key} ms"] = \
                            float(value)
            windows.append(window)
            if t := TICKS_RE.search(line):
                releases = int(t["releases"])
                for bucket in t["ticks"].split():
                    name, _, share = bucket.partition("=")
                    ticks[name] += releases * float(share.rstrip("%")) / 100.0
        elif m := HFR_RE.search(line):
            if m["text"] not in hfr_notes:
                hfr_notes.append(m["text"])
        elif m := POLICY_RE.search(line):
            policy = (int(m["sim"]), int(m["pres"]))

    print(f"log: {path}")
    if policy:
        print(f"frame policy: simulation {policy[0]} Hz, presentation {policy[1]} Hz")
    for note in phase4_notes:
        print(f"  phase4: {note}")
    if gpu_profile:
        report_gpu_profile(gpu_profile)
    if guest_summaries:
        report_guest_profile(guest_summaries, guest_entries, guest_callers)
    if not perf:
        print("No 'Vulkan perf' lines: was logging enabled and did the game reach gameplay?")
        return 1

    print(f"\n{len(perf)} 'Vulkan perf' windows of 120 presented frames")
    print(f"  fps              {describe([p['fps'] for p in perf])}")
    print(f"  ms/frame         {describe([p['frame'] for p in perf])}")
    print(f"  present ms/frame {describe([p['present'] for p in perf])}")
    print(f"  fence ms/frame   {describe([p['fence'] for p in perf])}")
    buckets = Counter(int(p["fps"] // 5 * 5) for p in perf)
    print("  fps histogram    " + "  ".join(f"{b}-{b + 4}:{buckets[b]}" for b in sorted(buckets)))

    if cpu:
        print("\nVulkan thread per frame (ms), 'Vulkan CPU profile':")
        for key in ("drawTotal", "drawOther", "attach", "tex", "pipe", "const", "vertex",
                    "resolve", "edramXfer"):
            print(f"  {key:<10} {describe([c[key] for c in cpu])}")
        print(f"  draws/frame {describe([c['draws'] for c in cpu])}")
        print(f"  resolves/f  {describe([c['resolves'] for c in cpu])}")

    # The executor's worker time is cumulative in the gpu-probe line (every 60
    # guest swaps). Worker ms per frame divided by the frame time is the share
    # of wall time the Vulkan thread was busy.
    utilizations, workers, backpressures = [], [], []
    for (f0, b0, w0), (f1, b1, w1), window in zip(probes, probes[1:], range(len(probes))):
        frames = f1 - f0
        if frames <= 0 or w1 < w0:
            continue
        worker = (w1 - w0) / frames
        backpressure = max(0.0, b1 - b0) / frames
        workers.append(worker)
        backpressures.append(backpressure)
        frame_ms = perf[min(len(perf) - 1, window * len(perf) // max(1, len(probes)))]["frame"]
        if frame_ms > 0:
            utilizations.append(100.0 * worker / frame_ms)
    if workers:
        print("\nVulkan executor thread (from cumulative 'gpu-probe ... worker=' values):")
        print(f"  busy ms/frame          {describe(workers)}")
        print(f"  busy % of frame time   {describe(utilizations)}")
        print(f"  PM4 backpressure ms/f  {describe(backpressures)}")
        median_util = statistics.median(utilizations) if utilizations else 0.0
        if median_util >= 85.0:
            verdict = ("the Vulkan thread is busy for almost the whole frame: it is the limiting "
                       "stage in these scenes")
        elif median_util >= 60.0:
            verdict = ("the Vulkan thread is busy for most of the frame: it is a likely limit; "
                       "confirm with the [pipeline] line of an hfr-unlimited build")
        else:
            verdict = ("the Vulkan thread is idle for a large part of every frame: the limit is "
                       "upstream (guest main thread or PM4/snapshot thread); the [pipeline] line "
                       "of an hfr-unlimited build names which")
        print(f"  => median {median_util:.0f}%: {verdict}.")

    if health:
        print(f"\npipelines={health[0]} shader modules={health[1]} (last 'Vulkan raster health')")

    if pipeline:
        print(f"\n{len(pipeline)} '[pipeline]' windows (hfr-unlimited build):")
        print(f"  fps    {describe([p[0] for p in pipeline])}")
        counts = Counter(p[2] for p in pipeline)
        total = sum(counts.values())
        print("  busiest stage: " + ", ".join(f"{name} {100 * n / total:.0f}% of windows"
                                             for name, n in counts.most_common()))
        print("  per frame:")
        for name, values in fields.items():
            if values:
                print(f"    {name:<20} {describe(values)}")
        group_names = list(dict.fromkeys(key for w in windows for key in w
                                         if any(key.startswith(g) for g in GROUP_RES)))
        for name in group_names:
            values = [w[name] for w in windows if name in w]
            print(f"    {name:<20} {describe(values)}")
        total_ticks = sum(ticks.values())
        if total_ticks:
            print("  title update intervals (16.667 ms ticks): " + "  ".join(
                f"{name}:{100 * n / total_ticks:.0f}%" for name, n in ticks.items()))
        report_window_class(windows, "slow windows (fps < 50)", lambda fps: fps < 50.0,
                            group_names)
        report_window_class(windows, "full-rate windows (fps >= 55)", lambda fps: fps >= 55.0,
                            group_names)
    if hfr_notes:
        print("\n[hfr] lines:")
        for note in hfr_notes:
            print(f"  {note}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
