#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
"""Pin PR59 driver bytes; distinguish inherited baseline drift from branch regressions."""
from pathlib import Path
import hashlib
import json
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
marker = ROOT / '.kiiko-source-view.json'
SOURCE = Path(json.loads(marker.read_text())['source']) if marker.exists() else ROOT
j = json.loads((SOURCE / 'tools/fixtures/pr59-driver-baseline.json').read_text())
overrides = j.get('approved_overrides', {})
assert set(overrides) <= set(j['sha256'])

base_ref = sys.argv[1] if len(sys.argv) > 1 else None
inherited = []
mismatches = []
for name, digest in j['sha256'].items():
    expected = overrides[name]['sha256'] if name in overrides else digest
    actual = hashlib.sha256((SOURCE / name).read_bytes()).hexdigest()
    if actual == expected:
        continue

    item = {'path': name, 'expected': expected, 'actual': actual}
    if base_ref:
        diff = subprocess.run(
            ['git', 'diff', '--quiet', f'{base_ref}...HEAD', '--', name],
            cwd=SOURCE,
            check=False,
        )
        if diff.returncode == 0:
            inherited.append(item)
            continue
        if diff.returncode != 1:
            raise subprocess.CalledProcessError(diff.returncode, diff.args)
    mismatches.append(item)

if inherited:
    print('PR59_BASELINE_INHERITED_DRIFT=' + json.dumps(inherited, ensure_ascii=False), flush=True)
if mismatches:
    print('PR59_BASELINE_NEW_MISMATCHES=' + json.dumps(mismatches, ensure_ascii=False), flush=True)
    raise AssertionError('PR59 driver baseline changed by this branch')

if base_ref:
    print(
        'PASS:',
        len(j['sha256']) - len(overrides),
        'PR59 driver/config/waveform files remain pinned; inherited baseline drift:',
        len(inherited),
        flush=True,
    )
else:
    print(
        'PASS:',
        len(j['sha256']) - len(overrides),
        'PR59 driver/config/waveform files and',
        len(overrides),
        'explicitly approved overrides match',
    )
