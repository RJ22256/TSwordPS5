"""Write the runtime's RomFS index (MacNativeGames' runtime-index.bin layout, "TOTKRFS1") from the verified index.json.

usage: python tools/make_romfs_index.py work/extracted/v143/romfs
Layout: 8-byte magic, u64 file count, u64 strings offset, then per file (sorted by path, byte order)
{u64 offset in romfs.bin, u64 size, u32 name offset, u32 name length including the NUL}, then the NUL-terminated paths.
"""
import json
import pathlib
import struct
import sys


def main():
    romfs = pathlib.Path(sys.argv[1])
    index = json.loads((romfs / 'index.json').read_text())
    if index.get('status') != 'pass':
        sys.exit('RomFS has not passed integrity verification')
    files = sorted(index['files'], key=lambda f: f['path'].encode('utf-8'))
    strings = bytearray()
    entries = bytearray()
    for f in files:
        name = f['path'].encode('utf-8') + b'\0'
        entries += struct.pack('<QQII', f['offset'], f['size'], len(strings), len(name))
        strings += name
    header = b'TOTKRFS1' + struct.pack('<QQ', len(files), 24 + len(entries))
    out = romfs / 'runtime-index.bin'
    out.write_bytes(header + entries + strings)
    print(f'{len(files)} files, {out.stat().st_size} bytes -> {out}')


if __name__ == '__main__':
    main()
