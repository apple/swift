#!/usr/bin/env python3
#
# Usage: ExtractOutputKey.py file.json OutputPath

import json
import sys

input_json = sys.argv[1]
output_path = sys.argv[2]


with open(input_json, 'r') as file:
    outputs = json.load(file)
    for output in outputs:
        if output['OutputPath'] != output_path:
            continue
        print(output['CacheKey'])
