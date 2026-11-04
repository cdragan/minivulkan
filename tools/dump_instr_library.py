#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

"""Print the records of an instrument library as one JSON array for jq."""

import json
import struct
import sys

data = open(sys.argv[1], 'rb').read()
count = struct.unpack_from('<I', data, 8)[0]
offset = 12
records = []
for index in range(count):
    category, name, size = struct.unpack_from('<24s24sI', data, offset)
    offset += 52
    records.append({
        'index': index,
        'category': category.split(b'\0')[0].decode(),
        'name': name.split(b'\0')[0].decode(),
        'payload': json.loads(data[offset:offset + size]),
    })
    offset += size
json.dump(records, sys.stdout)
