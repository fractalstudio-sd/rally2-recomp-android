#!/usr/bin/env python3
"""Maintainer tool: extract one game's ROM layout (file names, offsets, CRC32s) from Supermodel's Games.xml into data/<game>.json.
Users never need this; the JSON is plain metadata (no ROM content)."""
import sys, json, xml.etree.ElementTree as ET
xml, game, out = sys.argv[1:4]
g = next(x for x in ET.parse(xml).getroot().iter('game') if x.get('name') == game)
regions = []
for r in g.find('roms').findall('region'):
    regions.append(dict(name=r.get('name'), stride=int(r.get('stride'), 0), chunk_size=int(r.get('chunk_size'), 0),
        byte_swap=r.get('byte_swap') == 'true',
        files=[dict(name=f.get('name'), offset=int(f.get('offset'), 0), crc32=int(f.get('crc32'), 0)) for f in r.findall('file')]))
json.dump(dict(game=game, title=g.find('identity/title').text, regions=regions), open(out, 'w'), indent=1)
