#!/usr/bin/env python3
"""Validate replay inputs by reading 14 header bytes; never load/run Htsim."""

import argparse
import csv
import json
import math
import struct
from pathlib import Path


def read_matrix(path):
    text = path.read_text()
    if text.lstrip().startswith('{'):
        obj = json.loads(text)
        values = next(obj[key] for key in ('global', 'matrix', 'adjacency_matrix') if key in obj)
        if values and isinstance(values[0], list):
            rows = values
        else:
            n = math.isqrt(len(values))
            if not n or n * n != len(values):
                raise ValueError('flat matrix must have N*N elements')
            rows = [values[i:i + n] for i in range(0, len(values), n)]
        if obj.get('config', {}).get('num_servers', len(rows)) != len(rows):
            raise ValueError('num_servers differs from matrix size')
    else:
        lines = [line.strip() for line in text.splitlines()
                 if line.strip() and not line.lstrip().startswith('#')]
        cells = [next(csv.reader([line])) if ',' in line else line.split() for line in lines]
        indexed = bool(cells and cells[0][0] == 'src/dst')
        if indexed:
            cells = cells[1:]
            for i, row in enumerate(cells):
                if int(row[0]) != i:
                    raise ValueError('matrix CSV row indices must be sequential')
            cells = [row[1:] for row in cells]
        rows = [[int(cell) for cell in row] for row in cells]
        if len(rows) == 1:
            values = rows[0]
            n = math.isqrt(len(values))
            if not n or n * n != len(values):
                raise ValueError('flat matrix must have N*N elements')
            rows = [values[i:i + n] for i in range(0, len(values), n)]
    n = len(rows)
    if not n or any(len(row) != n for row in rows):
        raise ValueError('matrix must be nonempty and square')
    if any(type(value) is not int or value < 0 for row in rows for value in row):
        raise ValueError('matrix counts must be nonnegative integers')
    if any(rows[i][i] != 0 for i in range(n)):
        raise ValueError('matrix diagonal must be zero')
    return rows


def check_inputs(goal, matrix_path, routes_path=None, map_path=None):
    # Match sim/lgs/Parser.hpp, including its max-index rather than count fields.
    with goal.open('rb') as handle:
        header = handle.read(14)
    if len(header) != 14:
        raise ValueError('truncated GOAL binary header')
    magic, ranks, max_cpu, max_nic = struct.unpack('<QIBB', header)
    if magic != 4223 or ranks == 0:
        raise ValueError('invalid GOAL binary header')
    matrix = read_matrix(matrix_path)
    n = len(matrix)
    if map_path:
        mapping = [int(value) for line in map_path.read_text().splitlines()
                   if line.strip() and not line.lstrip().startswith('#')
                   for value in line.replace(',', ' ').split()]
        if len(mapping) != ranks:
            raise ValueError(f'map has {len(mapping)} entries but GOAL has {ranks} ranks')
    else:
        if ranks % n:
            raise ValueError('provide a rank-server map for uneven placement')
        mapping = [rank // (ranks // n) for rank in range(ranks)]
    if any(server < 0 or server >= n for server in mapping):
        raise ValueError('rank-server map contains an out-of-range server')

    pairs = set()
    if routes_path:
        text = routes_path.read_text()
        if text.lstrip().startswith(('[', '{')):
            entries = json.loads(text)
            if not isinstance(entries, list):
                raise ValueError('use a JSON list of src/dst/paths entries')
        else:
            entries = []
            for row in csv.DictReader(text.splitlines()):
                path = row['path'].replace('->', ' ').replace(';', ' ').split()
                entries.append({'src': int(row['src']), 'dst': int(row['dst']),
                                'paths': [[int(hop) for hop in path]]})
        for entry in entries:
            src, dst = entry['src'], entry['dst']
            if type(src) is not int or type(dst) is not int or not (0 <= src < n and 0 <= dst < n):
                raise ValueError('route endpoint outside matrix')
            for path in entry['paths']:
                if (len(path) < 2 or path[0] != src or path[-1] != dst
                        or len(set(path)) != len(path)):
                    raise ValueError('route endpoints/length/loop invalid')
                if any(type(hop) is not int or not 0 <= hop < n for hop in path):
                    raise ValueError('route hop outside matrix')
                if any(not matrix[a][b] for a, b in zip(path, path[1:])):
                    raise ValueError('route uses a missing link')
                pairs.add((src, dst))
    else:
        for src in range(n):
            visited = {src}
            pending = [src]
            for node in pending:
                for dst, lanes in enumerate(matrix[node]):
                    if lanes and dst not in visited:
                        visited.add(dst)
                        pending.append(dst)
            pairs.update((src, dst) for dst in visited if src != dst)
    # Conservative check, without decoding messages: require every occupied
    # server pair in both directions, including reverse ACK traffic.
    occupied = set(mapping)
    if any((src, dst) not in pairs for src in occupied for dst in occupied if src != dst):
        raise ValueError('missing route between occupied servers (data or ACK direction)')
    return {'goal_ranks': ranks, 'cpus_per_goal_rank': max_cpu + 1,
            'logical_nics_per_goal_rank': max_nic + 1, 'servers': n,
            'rank_to_server': mapping, 'adjacency_matrix': matrix,
            'directed_lanes': sum(map(sum, matrix)),
            'status': 'input check only; binary compatibility and replay NOT tested'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--goal', type=Path, required=True)
    parser.add_argument('--matrix', type=Path, required=True)
    parser.add_argument('--routes', type=Path)
    parser.add_argument('--rank-server-map', type=Path)
    args = parser.parse_args()
    try:
        report = check_inputs(args.goal, args.matrix, args.routes, args.rank_server_map)
    except (OSError, ValueError, KeyError, StopIteration) as error:
        parser.exit(1, f'[ERROR] {error}\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
