r"""(TSword PS5) The PS5 export: an UltraTears game folder made into the folder TSword on the PS5 (TSwordPS5, PPSA99778)
plays from - copied to TSword/ on a USB drive or extended storage (or into the title folder).

    <UltraTears>\deps\python\python.exe tools\build_ps5.py --game <UltraTears folder> --out <folder> [--cpus N] [--no-data]
    python tools/build_ps5.py --id-only               # the game code id (TSwordPS5's ps5/ut/configure.py asks too)
    python tools/build_ps5.py --dry-run --sdk <payload SDK> --lld <ld.lld> --out <dir>   # TSwordPS5's own build

It runs from the TSword PS5 export folder (TSword-PS5-export-<release>.zip: this script, the lifter and the runtime
headers of the UltraTears commit the PS5 title was built from, and kit/), or from a TSwordPS5 checkout's ultratears/.
--game is an UltraTears game folder that its tools/setup.py has built (data/exefs, data/romfs, data/shaders,
data/textures, data/fonts; deps/: Python with the lifter's packages, clang, lld, ninja). Nothing in it is changed but
data/generated-ps5 and build-ps5/, which this adds.

  1. the code lifted again at the PS5's bases (data/generated-ps5; ULTRS_MAIN_BASE, tools/lifter/bases.py): the
     console's own system modules sit where the PC puts the game's main module, 0x8_0000_0000
  2. that C compiled for the console - clang's x86_64-sie-ps5 target, as the payload SDK's prospero-clang compiles the
     title (-femulated-tls, -fno-plt, IEEE denormals), Zen 2, against the console's C headers from the kit (include/) -
     and linked by ld.lld into one position-independent ELF shared object, <out>/tsgame.so (-Bsymbolic: it binds its
     own names and imports only what the title exports: the kit's exports.txt, checked after the link)
  3. the game data beside it, as the PC's game folder has it (<out>/data: exefs, romfs, shaders/translated.pack,
     textures, fonts); files already there with the same size and date are left alone

--dry-run (the title's build, without a game): every function the lifted C can reach from cpu.h and simd.h compiled
and linked for the console as the export would - <dir>/need.txt, what such a library imports (the title exports it), and
<dir>/include, the console headers it reads (the kit's include/ the export compiles against).

tsgame.so carries tsword_gamecode_id, and <out>/tsgame.so.id the same text: a hash over what the lifted code is made by
and of (the lifter, the runtime headers it includes) and the flags. The title was built against the same and compares at
start; a library of another release is refused there with a message, not run.
"""
import argparse
import hashlib
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))

ABI = 'tsword-ut-gamecode-1'
PS5_MAIN_BASE = 0x1800000000   # src/runtime/rt.h (__PROSPERO__)
TARGET_FLAGS = ['--target=x86_64-sie-ps5', '-fvisibility-nodllstorageclass=default', '-fno-stack-protector', '-fno-plt',
                '-femulated-tls', '-fdenormal-fp-math=ieee', '-fPIC', '-march=znver2', '-nostdlibinc']
# build_game.py GAME_CFLAGS for the console: frame pointers kept (the console's crash logs walk them)
GAME_FLAGS = ['-O2', '-g0', '-fno-strict-aliasing', '-fno-omit-frame-pointer', '-D_FILE_OFFSET_BITS=64', '-w']
# -m elf_x86_64 first: MSYS2's ld.lld (UltraTears' deps/clang64 on Windows) links like MinGW unless told the target
LINK_FLAGS = ['-m', 'elf_x86_64', '-shared', '-Bsymbolic', '--hash-style=both', '-z', 'noexecstack', '-z', 'max-page-size=0x4000',
              '--build-id=none']
DATA = ['exefs', 'romfs', 'shaders/translated.pack', 'shaders/translated', 'textures', 'fonts']


def text_digest(p):
    """sha256 of a text file with its line endings as LF (a Windows checkout or zip may have turned them to CRLF)."""
    return hashlib.sha256(pathlib.Path(p).read_bytes().replace(b'\r\n', b'\n')).hexdigest()


def gamecode_id(root=ROOT):
    """What the title and the library must agree on: the lifter, the runtime headers the lifted C includes, the flags."""
    root = pathlib.Path(root)
    files = sorted([*(root / 'tools/lifter').glob('*.py'), *(root / 'tools/lifter').glob('*.txt'),
                    root / 'tools/gen_game.py', *(root / 'src/runtime').glob('*.h')],
                   key=lambda p: p.relative_to(root).as_posix())
    h = hashlib.sha256(f'{ABI}\n{PS5_MAIN_BASE:#x}\n{" ".join(TARGET_FLAGS)}\n{" ".join(GAME_FLAGS)}\n'.encode())
    for p in files:
        h.update(f'{p.relative_to(root).as_posix()} {text_digest(p)}\n'.encode())
    return h.hexdigest()[:32]


def ut_version(root=ROOT):
    sys.path.insert(0, str(pathlib.Path(root) / 'tools'))
    try:
        import version
        return version.current()[0]
    except Exception:   # (no git, no VERSION.txt: the archive's describe)
        t = (pathlib.Path(root) / 'tools/archive-version.txt').read_text().split()
        return t[1].lstrip('v') if len(t) > 1 else '?'


def elf_undefined(path):
    """The names an ELF shared object imports: its dynamic symbols without a section (read here: UltraTears' Windows
    toolchain has no llvm-nm)."""
    import struct
    b = pathlib.Path(path).read_bytes()
    if b[:4] != b'\x7fELF' or b[4] != 2:
        sys.exit(f'build_ps5: {path} is not a 64-bit ELF file')
    shoff, = struct.unpack_from('<Q', b, 0x28)
    shentsize, shnum = struct.unpack_from('<HH', b, 0x3a)
    secs = [struct.unpack_from('<IIQQQQIIQQ', b, shoff + i * shentsize) for i in range(shnum)]
    names = set()
    for sec in secs:
        if sec[1] != 11:  # SHT_DYNSYM
            continue
        stroff = secs[sec[6]][4]
        for o in range(sec[4], sec[4] + sec[5], 24):
            st_name, st_info, st_other, st_shndx = struct.unpack_from('<IBBH', b, o)
            if st_name and st_shndx == 0:
                end = b.index(b'\0', stroff + st_name)
                names.add(b[stroff + st_name:end].decode())
    return sorted(names)


def find_llvm(game):
    """clang, ld.lld, llvm-nm and ninja: the game folder's (tools/setup.py installs them), else ULTRS_CLANG64 /
    ULTRS_LLVM, else MSYS2's CLANG64, else this system's."""
    exe = (lambda n: n + '.exe') if os.name == 'nt' else (lambda n: n)
    dirs = [game / 'deps/clang64/bin', game / 'deps/llvm/bin', os.environ.get('ULTRS_CLANG64'), os.environ.get('ULTRS_LLVM'),
            'C:/msys64/clang64/bin']
    for d in dirs:
        if d and (pathlib.Path(d) / exe('clang')).is_file() and (pathlib.Path(d) / exe('ld.lld')).is_file():  # (no llvm-nm needed)
            d = pathlib.Path(d)
            ninja = d / exe('ninja')
            if not ninja.is_file():
                ninja = pathlib.Path(shutil.which('ninja') or ninja)
            return d / exe('clang'), d / exe('ld.lld'), d / exe('llvm-nm'), ninja
    w = [shutil.which(n) for n in ('clang', 'ld.lld', 'llvm-nm', 'ninja')]
    if all(w):
        return tuple(map(pathlib.Path, w))
    sys.exit(f'build_ps5: clang, ld.lld, llvm-nm and ninja not found in {game}\\deps (run UltraTears\' tools/setup.py '
             f'there first: it installs them)')


def p(path):
    """A path in a Ninja build line: absolute, forward slashes, escaped."""
    return pathlib.Path(path).resolve().as_posix().replace('$', '$$').replace(' ', '$ ').replace(':', '$:')


def q(s):
    s = str(s).replace('$', '$$')
    return f'"{s}"' if ' ' in s else s


def copy_tree(src, dst):
    """src into dst, file by file: a file there already with the same size and date is left alone."""
    n = size = 0
    items = [src] if src.is_file() else [f for f in src.rglob('*') if f.is_file()]
    for f in items:
        t = dst if src.is_file() else dst / f.relative_to(src)
        st = f.stat()
        if t.is_file() and t.stat().st_size == st.st_size and abs(t.stat().st_mtime - st.st_mtime) < 2:
            continue
        t.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(f, t)
        os.utime(t, (st.st_atime, st.st_mtime))
        n, size = n + 1, size + st.st_size
    return n, size


def inline_functions(clang, include, probe):
    """The names of every function cpu.h and what it includes from src/runtime (simd.h, simd_fp.h) define."""
    rt = (ROOT / 'src/runtime').resolve()
    probe.write_text('#include "cpu.h"\n#include "simd.h"\n')
    r = subprocess.run([str(clang), *TARGET_FLAGS, '-isystem', str(include), '-I', str(rt), '-fsyntax-only',
                        '-Xclang', '-ast-dump=json', '-Xclang', '-ast-dump-filter=', str(probe)],
                       capture_output=True, text=True)
    if r.returncode:
        r = subprocess.run([str(clang), *TARGET_FLAGS, '-isystem', str(include), '-I', str(rt), '-fsyntax-only',
                            '-Xclang', '-ast-dump=json', str(probe)], capture_output=True, text=True)
    if r.returncode:
        sys.exit(f'build_ps5: the inline probe: {r.stderr[-2000:]}')
    names, cur = [], None
    for d in json.loads(r.stdout)['inner']:
        loc = d.get('loc', {})
        f = loc.get('file') or (loc.get('expansionLoc') or {}).get('file') or (loc.get('spellingLoc') or {}).get('file')
        if f:
            cur = f
        if (d.get('kind') == 'FunctionDecl' and cur and pathlib.Path(cur).resolve().is_relative_to(rt)
                and any(i.get('kind') == 'CompoundStmt' for i in d.get('inner', []))):
            names.append(d['name'])
    return list(dict.fromkeys(names))


def dry_run(a):
    sdk, out = pathlib.Path(a.sdk).resolve(), pathlib.Path(a.out).resolve()
    clang, nm = sdk / 'bin/clang', sdk / 'bin/llvm-nm'
    lld = a.lld or shutil.which('ld.lld') or str(sdk / 'bin/ld.lld')
    include = sdk / 'target/include'
    shutil.rmtree(out, ignore_errors=True)
    (out / 'obj').mkdir(parents=True)
    names = inline_functions(clang, include, out / 'probe.c')
    (out / 'inlines.c').write_text('#include "cpu.h"\n#include "simd.h"\n'
                                   '__attribute__((visibility("default"))) void *tsg_inlines[] = {\n'
                                   + ''.join(f'    (void *){n},\n' for n in names) + '};\n')
    (out / 'table.c').write_text('#include "cpu.h"\nstatic void f_1800000000(Cpu *c) { (void)c; }\n'
                                 'const uint64_t main_fn_addrs[1] = {0x1800000000ull};\n'
                                 'const GuestFn main_fn_ptrs[1] = {f_1800000000};\nconst uint64_t main_fn_count = 1;\n')
    (out / 'id.c').write_text('__attribute__((visibility("default"))) const char tsword_gamecode_id[] = "dry";\n')
    objs = []
    for src in ('inlines.c', 'table.c', 'id.c'):
        o = out / 'obj' / (src[:-2] + '.o')
        r = subprocess.run([str(clang), *TARGET_FLAGS, *GAME_FLAGS, '-isystem', str(include), '-I',
                            str(ROOT / 'src/runtime'), '-MD', '-MF', f'{o}.d', '-c', str(out / src), '-o', str(o)])
        if r.returncode:
            sys.exit(f'build_ps5: {src} did not compile for the PS5')
        objs.append(o)
    lib = out / 'tsgame.so'
    if subprocess.run([lld, *LINK_FLAGS, *map(str, objs), '-o', str(lib)]).returncode:
        sys.exit('build_ps5: the dry-run library did not link')
    imp = subprocess.run([str(nm), '-D', '--undefined-only', str(lib)], capture_output=True, text=True).stdout
    need = sorted({l.split()[-1] for l in imp.splitlines() if l.split()})
    (out / 'need.txt').write_text('\n'.join(need) + '\n')
    headers = set()
    for d in (out / 'obj').glob('*.o.d'):
        for tok in re.split(r'\s+', d.read_text().replace('\\\n', ' ')):
            tok = tok.rstrip(':')
            if tok.endswith('.h'):
                headers.add(pathlib.Path(tok).resolve())
    copied = 0
    for h in sorted(headers):
        if h.is_relative_to(include.resolve()):
            dst = out / 'include' / h.relative_to(include.resolve())
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(h, dst)
            copied += 1
    (out / 'gamecode.id').write_text(gamecode_id() + '\n')
    print(f'dry run: {len(names)} inline functions, {len(need)} imports ({" ".join(need)}), {copied} headers')
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--game', help='the UltraTears game folder (default: the folder this script is in, if it is one)')
    ap.add_argument('--out', help='the PS5 folder to write (e.g. E:\\TSword on a USB drive)')
    ap.add_argument('--kit', help='the TSword PS5 kit of the release (default: kit/ beside tools/)')
    ap.add_argument('--cpus', type=int, default=os.cpu_count() or 4)
    ap.add_argument('--no-data', action='store_true', help='the game code only (the data is on the drive already)')
    ap.add_argument('--id-only', action='store_true')
    ap.add_argument('--dry-run', action='store_true')
    ap.add_argument('--sdk')
    ap.add_argument('--lld')
    a = ap.parse_args()
    if a.dry_run:
        return dry_run(a)
    gid = gamecode_id()
    if a.id_only:
        print(gid)
        return 0
    if not a.out:
        ap.error('--out: the folder to write for the PS5 (e.g. E:\\TSword on a USB drive)')
    game = pathlib.Path(a.game or ROOT).resolve()
    out, kit = pathlib.Path(a.out).resolve(), pathlib.Path(a.kit or ROOT / 'kit').resolve()
    data = game / 'data'
    if not (data / 'exefs/main').is_file():
        sys.exit(f'build_ps5: {data / "exefs/main"} is missing: --game must be an UltraTears game folder that its '
                 f'tools/setup.py has built')
    for mod in ('lz4', 'capstone', 'unicorn'):
        try:
            __import__(mod)
        except ImportError:
            sys.exit(f'build_ps5: this Python ({sys.executable}) has no {mod}: run this script with the game folder\'s '
                     f'Python, {game / "deps" / "python" / ("python.exe" if os.name == "nt" else "bin/python3")}')
    if not (kit / 'exports.txt').is_file():
        sys.exit(f'build_ps5: {kit} is not a TSword PS5 kit (exports.txt, gamecode.id, include/)')
    ours = (ROOT / 'SOURCE').read_text().split('commit: ')[1][:7] if (ROOT / 'SOURCE').is_file() else None
    theirs = (game / 'VERSION.txt').read_text(errors='replace').strip() if (game / 'VERSION.txt').is_file() else ''
    if ours and theirs and ours not in theirs:
        print(f'build_ps5: note: the PS5 title is UltraTears commit {ours}; the game folder says "{theirs}". Its data '
              f'(romfs index, shader pack, textures) is used as it is: if the game misbehaves on the PS5, set the game '
              f'folder up with UltraTears {ours}.', flush=True)
    want = (kit / 'gamecode.id').read_text().strip() if (kit / 'gamecode.id').is_file() else None
    if want and want != gid:
        sys.exit(f'build_ps5: this folder makes game code {gid}, the kit is for {want}: use the export of the same '
                 f'TSword release as the title')
    clang, lld, nm, ninja = find_llvm(game)
    env = dict(os.environ, PATH=str(clang.parent) + os.pathsep + os.environ.get('PATH', ''))

    # 1. the code at the PS5's bases
    gen = data / 'generated-ps5'
    stamp = gen / 'lifted.id'
    if not stamp.is_file() or stamp.read_text().strip() != gid:
        print('build_ps5: translating the code for the PS5 (its own addresses)', flush=True)
        lift_env = dict(env, ULTRS_MAIN_BASE=hex(PS5_MAIN_BASE))
        r = subprocess.run([sys.executable, str(ROOT / 'tools/gen_game.py'), str(data / 'exefs'), str(gen)], env=lift_env)
        if r.returncode:
            sys.exit(r.returncode)
        stamp.write_text(gid + '\n')

    # 2. compiled for the console, one shared object
    build = game / 'build-ps5'
    build.mkdir(exist_ok=True)
    (build / 'id.c').write_text(f'__attribute__((visibility("default"))) const char tsword_gamecode_id[] = "{gid}";\n')
    inc = f'-isystem {q((kit / "include").as_posix())} -I {q((ROOT / "src/runtime").as_posix())}'
    lines = ['# Written by tools/build_ps5.py; do not edit.', f'builddir = {p(build)}', f'cc = {q(clang.as_posix())}',
             f'ld = {q(lld.as_posix())}', f'flags = {" ".join(TARGET_FLAGS)} {" ".join(GAME_FLAGS)} {inc}',
             f'ldflags = {" ".join(LINK_FLAGS)}', '',
             'rule cc', '  command = $cc $flags -MD -MF $out.d -c $in -o $out', '  deps = gcc', '  depfile = $out.d',
             '  description = CC $in', '',
             'rule link', '  command = $ld $ldflags @$out.rsp -o $out', '  rspfile = $out.rsp',
             '  rspfile_content = $in_newline', '  description = LINK $out', '']
    objs = []
    sources = [c for m in ('main', 'subsdk0', 'sdk') for c in sorted((gen / m).glob('*.c'))] + [build / 'id.c']
    if len(sources) < 4:
        sys.exit(f'build_ps5: no generated C in {gen}')
    for c in sources:
        o = build / 'obj' / c.parent.name / (c.stem + '.o')
        objs.append(o)
        lines.append(f'build {p(o)}: cc {p(c)}')
    lib = build / 'tsgame.so'
    lines += [f'build {p(lib)}: link ' + ' '.join(p(o) for o in objs), f'default {p(lib)}', '']
    (build / 'build.ninja').write_text('\n'.join(lines), encoding='utf-8')
    print(f'build_ps5: compiling {len(sources)} files for the PS5 ({a.cpus} at a time)', flush=True)
    r = subprocess.run([str(ninja), '-C', str(build), '-j', str(a.cpus)], env=env)
    if r.returncode:
        sys.exit(r.returncode)
    imports = elf_undefined(lib)
    have = set((kit / 'exports.txt').read_text().split())
    lack = [n for n in imports if n not in have]
    if lack:
        sys.exit(f'build_ps5: the game code imports {len(lack)} names TSword on the PS5 does not have: '
                 f'{" ".join(lack[:30])}')
    out.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(lib, out / 'tsgame.so')
    (out / 'tsgame.so.id').write_text(gid + '\n')
    print(f'build_ps5: {out / "tsgame.so"} ({lib.stat().st_size >> 20} MB, game code id {gid}, {len(imports)} imports)')

    # 3. the data
    if not a.no_data:
        for d in DATA:
            src = data / d
            if not src.exists():
                continue
            n, size = copy_tree(src, out / 'data' / d)
            print(f'build_ps5: data/{d}: {n} files copied ({size / 2**30:.1f} GiB)', flush=True)
    print(f'build_ps5: done. Copy {out} to TSword/ on a USB drive (exFAT) for the PS5, or point the title at it with '
          f'game-files.txt.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
