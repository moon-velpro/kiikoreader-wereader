#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
"""中文：固定用户选定PR59底层字节，允许界面独立演进。/ English: Pin user-selected PR59 driver bytes while UI evolves independently."""
from pathlib import Path
import hashlib,json
ROOT=Path(__file__).resolve().parents[1]
marker=ROOT/'.kiiko-source-view.json'
SOURCE=Path(json.loads(marker.read_text())['source']) if marker.exists() else ROOT
j=json.loads((SOURCE/'tools/fixtures/pr59-driver-baseline.json').read_text())
overrides=j.get('approved_overrides',{})
assert set(overrides)<=set(j['sha256'])
mismatches=[]
for name,digest in j['sha256'].items():
    expected=overrides[name]['sha256'] if name in overrides else digest
    actual=hashlib.sha256((SOURCE/name).read_bytes()).hexdigest()
    if actual!=expected:mismatches.append({'path':name,'expected':expected,'actual':actual})
if mismatches:
    print('PR59_BASELINE_MISMATCHES='+json.dumps(mismatches,ensure_ascii=False),flush=True)
    raise AssertionError('PR59 driver baseline mismatch')
print('PASS:',len(j['sha256'])-len(overrides),'PR59 driver/config/waveform files and',len(overrides),'explicitly approved overrides match')
