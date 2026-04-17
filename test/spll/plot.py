# This work is part of the White Rabbit project
#
# Copyright (C) 2026 CERN (www.cern.ch)
#
# Released according to the GNU GPL, version 2 or any later version.

import matplotlib.pyplot as plt
import sys

nb_boards=5

# CSV format:
# board-id, count, date, temp,  servo-state, dms, temp, ses,  delcnt

times = []
temp = []
bd = [{'ss': [], 'dms': [], 'temp': [], 'ses': [], 'delcnt': []}
      for i in range(nb_boards)]
dms = [(None, None)] * nb_boards

data = {}

lineno=0
try:
    while True:
        l = sys.stdin.readline()
        if l == '':
            break
        lineno += 1
        fields = l.split(' ')
        for f in fields:
            if f == '':
                continue
            (name, val) = f.split(':')
            if not (name in data):
                data[name] = []
            data[name].append(int(val))
except Exception as e:
    print(f'Failed at line {lineno} with {e}')
    print(f'Line: {l}')
    print(f'Field: {fields}')
    raise


# Adjust the number/layout of graphs according to the number of boards:
fig, ax = plt.subplots(len(data) - 1, 1, sharex=True)

idx = 0
for k, v in data.items():
    if k == 't':
        continue
    ax[idx].plot(data['t'], v, label=k)
    ax[idx].set_title(k)
    idx += 1


plt.tight_layout()

plt.show()
