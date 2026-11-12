"""Read-only access to a RomFS in either form the build meets:
  - a folder of plain files (the player's dump: romfs/ extracted by their own tools);
  - the packed image: romfs.bin with index.json (tools/pack_romfs.py).
files: {path: size}, paths with '/' as on the console; read(path) -> bytes; image: the romfs.bin path or None.
"""
import json
import os
import pathlib


class RomFS:
    def __init__(self, root):
        self.root = pathlib.Path(root)
        bin_path, index = self.root / 'romfs.bin', self.root / 'index.json'
        if bin_path.is_file() and index.is_file():
            self.image = bin_path
            rows = json.loads(index.read_text())['files']
            self.offsets = {r['path']: r['offset'] for r in rows}
            self.files = {r['path']: r['size'] for r in rows}
            self._stream = None
        else:
            self.image = None
            self.offsets = None
            self.files = {}
            base = len(str(self.root)) + 1
            for dirpath, _, names in os.walk(self.root):
                for n in names:
                    full = os.path.join(dirpath, n)
                    self.files[full[base:].replace('\\', '/')] = os.path.getsize(full)

    def offset(self, path):
        """The file's offset in romfs.bin (0 for a folder: tools then open it by its path)."""
        return self.offsets[path] if self.offsets else 0

    def read(self, path):
        if self.image is None:
            return (self.root / path).read_bytes()
        if self._stream is None:
            self._stream = open(self.image, 'rb')
        self._stream.seek(self.offsets[path])
        data = self._stream.read(self.files[path])
        if len(data) != self.files[path]:
            raise ValueError(f'short RomFS read: {path}')
        return data
