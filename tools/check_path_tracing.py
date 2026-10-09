#!/usr/bin/env python3
"""Check real sample grouping and closed-room leakage using native GPU captures."""
import argparse
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def read_ppm(path):
    with path.open('rb') as stream:
        if stream.readline().strip() != b'P6':
            raise ValueError('expected binary PPM')
        width, height = map(int, stream.readline().split())
        if stream.readline().strip() != b'255':
            raise ValueError('expected 8-bit PPM')
        data = stream.read()
    if len(data) != width * height * 3:
        raise ValueError('truncated capture')
    return width, height, data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--player', type=Path, default=ROOT / 'build/aspera_player')
    parser.add_argument('--output', type=Path, default=ROOT / 'out/path-tracing-checks')
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    for name, spp, samples, camera in (
            ('group-1', 1, 256, None), ('group-16', 16, 256, None),
            ('sealed-257', 64, 257, '-4,1,7,0,0'),
            ('reflection', 16, 256, '-1.4,1.2,-3,0,0')):
        command = [str(args.player.resolve()), '--project', str(ROOT / 'games/path-tracing'),
                   '--renderer', 'path-traced', '--width', '800', '--height', '600',
                   '--no-ui', '--no-bloom', '--pt-samples', str(spp),
                   '--pt-capture-samples', str(samples), '--screenshot', str(output / (name + '.ppm'))]
        if camera:
            command += ['--camera', camera]
        with (output / (name + '.log')).open('w') as log:
            subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT,
                           check=True, timeout=180)
    a, b, sealed = (read_ppm(output / (name + '.ppm'))
                    for name in ('group-1', 'group-16', 'sealed-257'))
    if a[:2] != b[:2]:
        raise ValueError('capture sizes differ')
    rw, rh, reflection = read_ppm(output / 'reflection.ppm')
    region = (rw // 2 - 20, rh // 2 - 20, rw // 2 + 20, rh // 2 + 20)
    reflected_rgb = [sum(reflection[(y * rw + x) * 3 + c]
                         for y in range(region[1], region[3])
                         for x in range(region[0], region[2])) / 1600 for c in range(3)]
    delta = sum(abs(x-y) for x, y in zip(a[2], b[2])) / len(a[2])
    report = {
        'checks': {'sample_grouping_equivalent': delta < .01,
                   'sealed_room_unlit': max(sealed[2]) == 0,
                   'fixture_visible': max(a[2]) > 32,
                   'off_camera_red_reflection': reflected_rgb[0] > max(1, reflected_rgb[1], reflected_rgb[2]) * 2},
        'grouping_mean_channel_difference_0_to_255': delta,
        'sealed_max_channel_0_to_255': max(sealed[2]),
        'off_camera_reflection_mean_rgb_0_to_255': reflected_rgb,
        'note': '257 samples with 64 spp exercises a final partial-sample dispatch. These checks do not qualify performance or arbitrary scene convergence.'}
    (output / 'checks.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
    return 0 if all(report['checks'].values()) else 1


if __name__ == '__main__':
    raise SystemExit(main())
