"""RomFS recon: file types and sizes, plus the shader inventory from MacNativeGames' prepare_shader_seed.

usage: python tools/recon_romfs.py work/extracted/v143/romfs --output work/analysis/v143-romfs.json [--no-shaders]
"""
import argparse
import collections
import json
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'refs/MacNativeGames/games/totk/scripts'))


def kind(path):
    name = path.rsplit('/', 1)[-1]
    if name.endswith('.zs'):
        name = name[:-3]
    parts = name.split('.')
    return '.'.join(parts[-2:]) if len(parts) > 2 else (parts[-1] if len(parts) > 1 else '(none)')


def main():
    p = argparse.ArgumentParser()
    p.add_argument('romfs', type=pathlib.Path)
    p.add_argument('--output', type=pathlib.Path, required=True)
    p.add_argument('--no-shaders', action='store_true')
    a = p.parse_args()
    index = json.loads((a.romfs / 'index.json').read_text())
    files = index['files']
    by_kind = collections.defaultdict(lambda: [0, 0])
    by_top = collections.defaultdict(lambda: [0, 0])
    for f in files:
        k = by_kind[kind(f['path'])]; k[0] += 1; k[1] += f['size']
        t = by_top[f['path'].split('/', 1)[0]]; t[0] += 1; t[1] += f['size']
    total = sum(f['size'] for f in files)
    report = dict(files=len(files), bytes=total, romfs_sha256=index['romfs_sha256'],
                  by_top_dir={k: dict(files=v[0], mib=round(v[1] / 2**20, 1)) for k, v in
                              sorted(by_top.items(), key=lambda kv: -kv[1][1])},
                  by_type={k: dict(files=v[0], mib=round(v[1] / 2**20, 1)) for k, v in
                           sorted(by_kind.items(), key=lambda kv: -kv[1][1])[:40]})
    if not a.no_shaders:
        import prepare_shader_seed as seed
        _, shaders, startup, archives = seed.inventory(a.romfs)
        stages = collections.Counter(stage for stage, *_ in shaders.values())
        report['shaders'] = dict(unique=len(shaders), default_bind_state=len(startup), archives=len(archives),
                                 by_stage=dict(stages))
    a.output.parent.mkdir(parents=True, exist_ok=True)
    a.output.write_text(json.dumps(report, indent=1))
    print(json.dumps({k: v for k, v in report.items() if k not in ('by_type',)}, indent=1)[:4000])


if __name__ == '__main__':
    main()
