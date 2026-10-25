#!/usr/bin/env python3
"""Light.log DIAG deltas; no driver hooks, works with unchanged 0.2.9 logs."""
import argparse,re,pathlib
parser=argparse.ArgumentParser(description='Inspect per-interval light-pass pressure')
parser.add_argument('logs',nargs='+',help='Light.log from separate full game sessions')
args=parser.parse_args()
keys=('draw','nativeSingle','nativeUber','replaySingles','replayUberBatches','replayedMeshes','replayedOverflow','overflowEligible','skippedNoDevice','stockOnly','additiveStateAttempts','additiveStateOK')
for fn in args.logs:
  src=pathlib.Path(fn).read_text(encoding='utf-8',errors='replace')
  samples=[]
  for line in src.splitlines():
    if 'DIAG ' not in line:continue
    data={k:int(v) for k,v in re.findall(r'([A-Za-z0-9_]+)=(\d+)',line)}
    if 'draw' in data:samples.append(data)
  print('\n'+fn,'samples',len(samples))
  if len(samples)<2:
    print('  Need LogLights=1 and at least two DIAG lines (5 second sampling).')
    continue
  first,last=samples[0],samples[-1]
  for k in keys:
    if k in first and k in last:
      diff=last[k]-first[k]
      print('  %-20s %12d cumulative delta over %d intervals'% (k,diff,len(samples)-1))
  draw=last['draw']-first['draw']
  if draw>0:
    for key in ('replaySingles','replayUberBatches','replayedMeshes','overflowEligible'):
      if key in first and key in last:
        print('  %-20s %12.3f per draw'% (key,(last[key]-first[key])/draw))
  print('  GPU work not measured by this log. Compare actual FPS separately.')
