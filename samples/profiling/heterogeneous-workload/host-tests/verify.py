#!/usr/bin/env python3
"""Validate retained sample CSV; does not certify a profiler wire capture."""
import argparse
import csv
from pathlib import Path


def validate(path, workers):
    if not 1 <= workers <= 6:
        raise ValueError('invalid worker count')
    with path.open(newline='') as stream:
        rows = list(csv.DictReader(stream))
    pending = {}
    seen = set()
    frames = 0
    sequence = 0
    ended = False
    gpu_available = 0
    for row in rows:
        if ended:
            raise ValueError('records after END')
        kind = row['kind']
        seq = int(row['sequence'])
        phase = int(row['phase'])
        if kind == 'SPE':
            lane = int(row['lane'])
            if seq <= sequence or lane in pending or not 0 <= lane < workers:
                raise ValueError('stale/duplicate/invalid worker')
            if phase not in (1, 2) or int(row['frame']) != frames:
                raise ValueError('worker phase/frame mismatch')
            pending[lane] = row
        elif kind == 'FRAME':
            if seq != sequence + 1 or int(row['frame']) != frames or phase not in range(4):
                raise ValueError('frame identity')
            if int(row['items']) != 16384 or int(row['digest'], 16) != 0xe7b15af8:
                raise ValueError('compute mismatch')
            if int(row['compute_us']) <= 0 or int(row['frame_us']) < int(row['compute_us']):
                raise ValueError('elapsed time invalid')
            if phase in (1, 2):
                if set(pending) != set(range(workers)):
                    raise ValueError('missing worker')
                digest = 0
                for lane, entry in pending.items():
                    if int(entry['sequence']) != seq or int(entry['phase']) != phase:
                        raise ValueError('worker correlation mismatch')
                    if int(entry['items']) != len(range(lane, 16384, workers)):
                        raise ValueError('partition count')
                    digest ^= int(entry['digest'], 16)
                if digest != 0xe7b15af8:
                    raise ValueError('worker digest')
            elif pending:
                raise ValueError('worker record in PPU phase')
            valid = int(row['gpu_valid'])
            if valid not in (0, 1):
                raise ValueError('invalid GPU validity marker')
            if valid:
                if not 0 < int(row['gpu_begin']) <= int(row['gpu_end']):
                    raise ValueError('GPU report invalid')
                gpu_available += 1
            pending.clear()
            seen.add(phase)
            sequence = seq
            frames += 1
        elif kind == 'END':
            if pending or int(row['digest']) != 0 or int(row['frame']) != frames or int(row['items']) != frames:
                raise ValueError('bad finalization')
            if phase != 15 or seen != {0, 1, 2, 3}:
                raise ValueError('incomplete phase coverage')
            ended = True
        else:
            raise ValueError('unknown record kind')
    if not ended or not frames:
        raise ValueError('missing END/workload')
    return frames, gpu_available


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('csv', type=Path)
    parser.add_argument('--workers', type=int, required=True)
    args = parser.parse_args()
    try:
        count, gpu = validate(args.csv, args.workers)
        print(f'SAMPLE_CSV PASS frames={count} phases=4 workers={args.workers} gpu_available={gpu}')
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f'SAMPLE_CSV FAIL {error}')
        raise SystemExit(1)
