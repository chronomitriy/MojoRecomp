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


def percentile(values: list[float], fraction: float) -> float:
    ordered = sorted(values)
    index = min(len(ordered) - 1, max(0, round(fraction * (len(ordered) - 1))))
    return ordered[index]


def describe(values: list[float]) -> str:
    if not values:
        return "n/a"
    return (f"median {statistics.median(values):6.2f}  p10 {percentile(values, 0.1):6.2f}  "
            f"p90 {percentile(values, 0.9):6.2f}")


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(__doc__.strip())
        return 2
    path = Path(argv[1])
    text = path.read_text(encoding="utf-8", errors="replace").splitlines()

    perf, cpu, probes, pipeline = [], [], [], []
    health = None
    policy = None
    for line in text:
        if m := PERF_RE.search(line):
            perf.append({k: float(v) for k, v in m.groupdict().items()})
        elif m := CPU_RE.search(line):
            cpu.append({k: float(v) for k, v in m.groupdict().items()})
        elif m := PROBE_RE.search(line):
            probes.append((int(m["frame"]), float(m["backpressure"]), float(m["worker"])))
        elif m := HEALTH_RE.search(line):
            health = (int(m["pipelines"]), int(m["modules"]))
        elif m := PIPELINE_RE.search(line):
            pipeline.append((float(m["fps"]), float(m["frame"]), m["busiest"], int(m["share"])))
        elif m := POLICY_RE.search(line):
            policy = (int(m["sim"]), int(m["pres"]))

    print(f"log: {path}")
    if policy:
        print(f"frame policy: simulation {policy[0]} Hz, presentation {policy[1]} Hz")
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
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
