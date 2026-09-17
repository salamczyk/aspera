#!/usr/bin/env python3
"""Capture deterministic lighting transitions with bloom disabled and check temporal cleanup."""
import argparse
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def ppm(path):
    with path.open("rb") as stream:
        if stream.readline().strip() != b"P6":
            raise ValueError(f"not a binary PPM: {path}")
        width, height = map(int, stream.readline().split())
        if stream.readline().strip() != b"255":
            raise ValueError("expected 8-bit capture")
        pixels = stream.read()
    if len(pixels) != width * height * 3:
        raise ValueError("truncated capture")
    return width, height, pixels


def difference(a, b):
    if a[:2] != b[:2]:
        raise ValueError("capture sizes differ")
    return sum(abs(x - y) for x, y in zip(a[2], b[2])) / len(a[2])


def region_mean(capture, rect):
    width, _, pixels = capture
    x0, y0, x1, y1 = rect
    n = (x1 - x0) * (y1 - y0)
    return [sum(pixels[(y * width + x) * 3 + c]
                for y in range(y0, y1) for x in range(x0, x1)) / n for c in range(3)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--player", type=Path, default=ROOT / "build" / "aspera_player")
    parser.add_argument("--output", type=Path, default=ROOT / "out" / "lighting-checks")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    frames = (30, 126, 130, 246, 260)
    captures = {}
    for frame in frames:
        path = args.output.resolve() / f"indirect-{frame}.ppm"
        command = [str(args.player.resolve()), "--project", str(ROOT / "games" / "lighting-bounce"),
                   "--width", "800", "--height", "600", "--fixed-step", "--no-ui", "--no-bloom",
                   "--debug-view", "indirect", "--frames", str(frame), "--screenshot", str(path)]
        print(f"Capture indirect lighting at fixed frame {frame}", flush=True)
        subprocess.run(command, cwd=ROOT, check=True)
        captures[frame] = ppm(path)
    wall_delta = difference(captures[30], captures[130])
    wall_settle = difference(captures[126], captures[130])
    emission_delta = difference(captures[130], captures[260])
    emission_settle = difference(captures[246], captures[260])
    red_floor = region_mean(captures[260], (285, 285, 330, 300))
    blue_floor = region_mean(captures[260], (520, 329, 555, 345))
    checks = {"wall_removal_changes_indirect": wall_delta > .05,
              "wall_settles_by_four_frames_after_commit": wall_settle < .5,
              "emission_illuminates_with_bloom_disabled": emission_delta > .2,
              "emission_extinction_settles_within_four_frames": emission_settle < .5,
              "red_diffuse_bounce": red_floor[0] > max(1, red_floor[2]) * 1.15,
              "blue_diffuse_bounce": blue_floor[2] > max(1, blue_floor[0]) * 1.15}
    report = {"checks": checks, "mean_rgb_difference_0_to_255": {
        "wall_removal": wall_delta, "wall_settle": wall_settle,
        "emission_extinction": emission_delta, "emission_settle": emission_settle},
        "neutral_floor_rgb_after_emitter_off": {"near_red_wall": red_floor, "near_blue_wall": blue_floor},
        "capture_note": "The screenshot is requested two loop frames before --frames. Wall damage is asynchronous; the checks allow its command commit and four rendered frames."}
    (args.output / "checks.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2), flush=True)
    return 0 if all(checks.values()) else 1


if __name__ == "__main__":
    raise SystemExit(main())
