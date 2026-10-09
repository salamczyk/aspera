#!/usr/bin/env python3
"""Run the sustained native graphics suite; retain raw frames, pass timings, and hardware data."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
SCENES = ("demolition", "construction", "collision", "lighting-benchmark")


def hardware():
    data = {"os": platform.platform(), "architecture": platform.machine(),
            "logical_cpus": os.cpu_count(), "gpu": "unavailable"}
    try:
        if sys.platform == "darwin":
            data["cpu"] = subprocess.check_output(
                ["sysctl", "-n", "machdep.cpu.brand_string"], text=True).strip()
            info = json.loads(subprocess.check_output(
                ["system_profiler", "SPDisplaysDataType", "-json"], text=True))
            data["gpu"] = [{k: x[k] for k in ("sppci_model", "spdisplays_vendor",
                                              "spdisplays_vram", "spdisplays_vram_shared") if k in x}
                           for x in info.get("SPDisplaysDataType", [])]
        elif sys.platform == "win32":
            data["gpu"] = json.loads(subprocess.check_output([
                "powershell", "-NoProfile", "-Command",
                "Get-CimInstance Win32_VideoController | Select-Object Name,DriverVersion,AdapterRAM | ConvertTo-Json"
            ], text=True))
        else:
            data["gpu"] = subprocess.check_output(["lspci", "-mm"], text=True).splitlines()
            data["gpu"] = [x for x in data["gpu"] if "VGA" in x or "3D controller" in x]
    except (OSError, subprocess.SubprocessError, ValueError) as error:
        data["query_error"] = str(error)
    return data


def source_digest():
    digest = hashlib.sha256()
    for directory in ("src", "include", "assets/shaders", "games"):
        for path in sorted((ROOT / directory).rglob("*")):
            if path.is_file() and path.suffix in (".cpp", ".mm", ".hpp", ".sc", ".sh", ".lua", ".metal", ".glsl", ".comp", ".vert", ".frag"):
                digest.update(path.relative_to(ROOT).as_posix().encode())
                digest.update(path.read_bytes())
    return digest.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--player", type=Path, default=ROOT / "build" / "aspera_player")
    parser.add_argument("--output", type=Path, default=ROOT / "out" / "render-benchmarks")
    parser.add_argument("--scenes", nargs="+", choices=SCENES, default=list(SCENES))
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--seconds", type=int, default=60)
    parser.add_argument("--warmup", type=int, default=5)
    parser.add_argument("--quality", choices=("balanced", "low"), default="balanced")
    parser.add_argument("--renderer", choices=("raster", "path-traced"), default="raster")
    parser.add_argument("--require-local-gate", action="store_true")
    args = parser.parse_args()
    if args.runs < 1 or args.seconds < 1 or args.warmup < 0:
        parser.error("runs/seconds must be positive and warmup nonnegative")
    player = args.player.resolve()
    if not player.is_file():
        parser.error(f"player not found: {player}")
    args.output.mkdir(parents=True, exist_ok=True)
    output = args.output.resolve()
    if (output / "suite.json").exists():
        parser.error("output already contains a suite; choose a new directory to retain prior results")
    suite = {"started_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
             "hardware": hardware(), "source_sha256": source_digest(), "runs": [],
             "renderer": args.renderer,
             "target_hardware_gate": ("Unqualified until three passing runs per scene on 8-core M3 and RTX 3060 12 GB; Windows/Linux runtime checks required" if args.renderer == "path-traced" else "Unqualified until three passing runs per scene on both M1 and GTX 1660")}
    (output / "suite.json").write_text(json.dumps(suite, indent=2) + "\n")
    for scene in args.scenes:
        for run in range(1, args.runs + 1):
            name = f"{scene}-{run}"
            csv = output / f"{name}.csv"
            command = [str(player), "--project", str(ROOT / "games" / scene),
                       "--render-benchmark", str(csv), "--width", "1920", "--height", "1080",
                       "--renderer", args.renderer, "--quality", args.quality, "--benchmark-seconds", str(args.seconds),
                       "--warmup-seconds", str(args.warmup)]
            print(f"{name}: {args.seconds}s after {args.warmup}s warm-up at 1920x1080 {args.quality}", flush=True)
            with (output / f"{name}.log").open("w") as log:
                result = subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
            if result.returncode:
                raise SystemExit(f"{name} failed ({result.returncode}); see {output / (name + '.log')}")
            report = json.loads(csv.with_suffix(".json").read_text())
            suite["runs"].append({"scene": scene, "run": run, "report": report})
            (output / "suite.json").write_text(json.dumps(suite, indent=2) + "\n")
            frame = report.get("frame_ms") or {}
            gi = report.get("gi_ms") or {}
            print(f"  frame p95 {frame.get('p95')} ms; GI p95 {gi.get('p95')} ms", flush=True)
    suite["local_frame_gate_passed"] = (args.runs >= 3 and args.seconds >= 60 and
        set(args.scenes) == set(SCENES) and all(x["report"]["local_frame_gate_passed"] for x in suite["runs"]))
    suite["finished_utc"] = datetime.datetime.now(datetime.timezone.utc).isoformat()
    (output / "suite.json").write_text(json.dumps(suite, indent=2) + "\n")
    lines = ["# Native graphics measurements", "", "```json", json.dumps(suite["hardware"], indent=2), "```", "",
             "| Scene | Run | Frame p50/p95/p99 ms | GPU p95 ms | GI p95 ms | Textures MiB | Local frame gate |",
             "|---|---:|---|---:|---:|---:|---|"]
    for item in suite["runs"]:
        report = item["report"]
        frame = report["frame_ms"]
        def value(metric):
            return f"{report[metric]['p95']:.3f}" if report.get(metric) else "unavailable"
        lines.append(f"| {item['scene']} | {item['run']} | {frame['p50']:.3f}/{frame['p95']:.3f}/{frame['p99']:.3f} | "
                     f"{value('gpu_ms')} | {value('gi_ms')} | {report['texture_peak_bytes']/1048576:.1f} | "
                     f"{report['local_frame_gate_passed']} |")
    lines += ["", "GI is the GPU interval from the first occupancy/injection dispatch through bilateral upsampling.",
              "Individual view intervals can overlap on tile GPUs and must not be summed.",
              "Texture memory includes render targets; GI memory in JSON overlaps that texture estimate.",
              "Raw frame/pass CSVs retain outliers. The initial physics-cache build precedes the timed warm-up.",
              "", suite["target_hardware_gate"], ""]
    (output / "REPORT.md").write_text("\n".join(lines))
    print(f"Report: {output / 'REPORT.md'}", flush=True)
    if args.require_local_gate and not suite["local_frame_gate_passed"]:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
