"""(TSword) The game code for TSword on the PS5: the lifted C of the player's update, its modules.c and the install
side (romfs.c, patches.c, ov_zstd.c, mouse_direct.c) compiled for the console and linked into one ELF shared object,
<out>/tsgame.so, which the PS5 title (TSwordPS5, PPSA99778) loads at start (its gamecode.c and elfload.c).

    python tools/build_ps5.py --gen DIR --out DIR --toolchain LLVM --runtime DIR --ps5 DIR --version v143
                              --expected JSON --romfs DIR [--ninja EXE] [--jobs N] [--pgo FILE]
    python tools/build_ps5.py --id-only --runtime DIR        # the game code id (TSwordPS5's configure.py asks too)
    python tools/build_ps5.py --dry-run --sdk SDK --runtime DIR --deps DIR --out DIR   # TSwordPS5's CI, see below

Same C as the PC's build (tools/build_game.py), other target: the console's (clang's x86_64-sie-ps5, as the PS5
payload SDK's prospero-clang compiles the title: -femulated-tls, -fno-plt, IEEE denormals), its CPU (Zen 2) and its
C library's headers (--ps5: the TSword PS5 kit of the release, ps5/include: FreeBSD's libc headers as the payload SDK
installs them and the zstd headers of the title's zstd), position-independent, linked by ld.lld with -Bsymbolic: the
library binds its own names, and imports only what the title exports (ps5/exports.txt; checked after the link, so a
mismatch stops here with the names rather than on the console).

The library carries tsword_gamecode_id, and <out>/tsgame.so.id the same text: a sha256 over the runtime headers and
the install-side sources (line endings ignored) and the flags, which the title was built against and compares at
start. A library from another release's headers is refused there with a message, not run.

--dry-run (TSwordPS5's CI, Linux, with the payload SDK): the install side with stand-in generated headers, a modules.c
of one module, and every inline function of cpu.h and simd.h (the code the lifted C is made of) built the same way
against the SDK's own headers, so that CI knows what such a library imports (<out>/need.txt, for the title's export
table) and which headers it reads (<out>/include: the kit's ps5/include, copied from the SDK).
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

ABI = 'tsword-gamecode-1'
INSTALL_SIDE = ('romfs.c', 'patches.c', 'ov_zstd.c', 'mouse_direct.c')
# the console's target, as the payload SDK's prospero-clang has it (its host/bin/prospero-clang)
TARGET_FLAGS = ['--target=x86_64-sie-ps5', '-fvisibility-nodllstorageclass=default', '-fno-stack-protector', '-fno-plt',
                '-femulated-tls', '-fdenormal-fp-math=ieee', '-fPIC', '-march=znver2', '-nostdlibinc']
# build_game.py GEN_FLAGS and RT_FLAGS with the console's CPU (RT_CPU_LEVEL 3: AVX2 and FMA, as the title's runtime)
GEN_FLAGS = ['-O1', '-fno-strict-aliasing', '-Wno-return-type', '-Wno-unused-label', '-fvisibility=hidden']
RT_FLAGS = ['-O2', '-Wall', '-fno-strict-aliasing', '-DRT_CPU_LEVEL=3', '-Wno-unknown-warning-option']
LINK_FLAGS = ['-shared', '-Bsymbolic', '--hash-style=both', '-z', 'noexecstack', '-z', 'max-page-size=0x4000',
              '--build-id=none']
EXE = '.exe' if os.name == 'nt' else ''


def text_digest(p):
    """sha256 of a text file with its line endings as LF (a Windows checkout or zip may have turned them to CRLF)."""
    return hashlib.sha256(Path(p).read_bytes().replace(b'\r\n', b'\n')).hexdigest()


def gamecode_id(runtime):
    """What the title and the library must agree on: the runtime's headers (runtime/*.h, runtime/platform/*.h), the
    install-side sources and the flags."""
    runtime = Path(runtime)
    files = sorted([*runtime.glob('*.h'), *(runtime / 'platform').glob('*.h'), *(runtime / n for n in INSTALL_SIDE)],
                   key=lambda p: p.relative_to(runtime).as_posix())
    h = hashlib.sha256(f'{ABI}\n{" ".join(TARGET_FLAGS)}\n{" ".join(RT_FLAGS)}\n'.encode())
    for p in files:
        h.update(f'{p.relative_to(runtime).as_posix()} {text_digest(p)}\n'.encode())
    return h.hexdigest()[:32]


def q(s):
    """A Ninja command argument (build_game.cmd_arg: escaped, quoted when it holds a space)."""
    s = str(s).replace('$', '$$')
    return f'"{s}"' if ' ' in s else s


def p(path):
    """A path in a Ninja build line: absolute, forward slashes, escaped."""
    return Path(path).resolve().as_posix().replace('$', '$$').replace(' ', '$ ').replace(':', '$:')


def qp(path):
    return q(Path(path).resolve().as_posix())


def ninja_text(clang, lld, sources, out, include, gen_flags, rt_flags, jobs, objs_out):
    lines = ['# Written by tools/build_ps5.py; do not edit.', 'ninja_required_version = 1.10',
             f'builddir = {p(out)}', f'cc = {qp(clang)}', f'ld = {qp(lld)}',
             f'target = {" ".join(TARGET_FLAGS)} {include}', f'genflags = {" ".join(gen_flags)}',
             f'rtflags = {" ".join(rt_flags)}', f'ldflags = {" ".join(LINK_FLAGS)}', '',
             'pool gen', f'  depth = {jobs}', '',
             'rule cc_gen', '  command = $cc $target $genflags -MD -MF $out.d -c $in -o $out', '  deps = gcc',
             '  depfile = $out.d', '  description = CC $in', '',
             'rule cc_rt', '  command = $cc $target $rtflags -MD -MF $out.d -c $in -o $out', '  deps = gcc',
             '  depfile = $out.d', '  description = CC $in', '',
             'rule link', '  command = $ld $ldflags @$out.rsp -o $out', '  rspfile = $out.rsp',
             '  rspfile_content = $in_newline', '  description = LINK $out', '']
    objs = []
    for src, kind, sub in sources:
        o = out / 'obj' / sub / (Path(src).stem + '.o')
        objs.append(o)
        lines.append(f'build {p(o)}: {kind} {p(src)}')
        if kind == 'cc_gen':
            lines.append('  pool = gen')
    lib = out / 'tsgame.so'
    lines += [f'build {p(lib)}: link ' + ' '.join(p(o) for o in objs), f'default {p(lib)}', '']
    objs_out.extend(objs)
    return '\n'.join(lines), lib


def run(cmd, **kw):
    print('+ ' + ' '.join(map(str, cmd)), flush=True)
    return subprocess.run([str(c) for c in cmd], **kw)


def imports_of(nm, lib):
    r = subprocess.run([str(nm), '-D', '--undefined-only', str(lib)], capture_output=True, text=True)
    if r.returncode:
        sys.exit(f'build_ps5: {nm} -D {lib}: {r.stderr.strip()}')
    return sorted({l.split()[-1] for l in r.stdout.splitlines() if l.split()})


def module_c(layout, linked, hashes, version, gid):
    import build_game
    paths = {k: Path('/app0') / k for k in ('exefs', 'romfs', 'profile')}
    text = build_game.module_glue(layout, linked, hashes, {k: PosixPath(v) for k, v in paths.items()}, version)
    return text + f'\n__attribute__((visibility("default"))) const char tsword_gamecode_id[] = "{gid}";\n'


class PosixPath:
    """A path module_glue writes as given (as_posix), whatever the OS this runs on."""
    def __init__(self, p):
        self.p = str(p).replace('\\', '/')

    def as_posix(self):
        return self.p


def write_if_changed(path, text):
    path = Path(path)
    if path.exists() and path.read_text(encoding='utf-8', errors='replace') == text:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding='utf-8', newline='\n')


def build(a):
    import build_game
    import romfs_index
    gen, out, runtime, kit = (Path(x).resolve() for x in (a.gen, a.out, a.runtime, a.ps5))
    llvm = Path(a.toolchain).resolve()
    clang, lld, nm = (llvm / 'bin' / (n + EXE) for n in ('clang', 'ld.lld', 'llvm-nm'))
    ninja = Path(a.ninja).resolve() if a.ninja else Path(shutil.which('ninja') or 'ninja')
    for tool in (clang, lld, nm, ninja):
        if not tool.is_file():
            sys.exit(f'build_ps5: {tool} not found')
    include = kit / 'include'
    exports_file = kit / 'exports.txt'
    want = (kit / 'gamecode.id').read_text(encoding='utf-8').strip() if (kit / 'gamecode.id').is_file() else None
    gid = gamecode_id(runtime)
    if want and want != gid:
        sys.exit(f'build_ps5: the runtime here is {gid}, the PS5 kit was made for {want}: not one TSword release')
    out.mkdir(parents=True, exist_ok=True)
    layout = build_game.read_layout(gen)
    linked = [n for n, _, _ in layout]
    mods = {m: build_game.chunk_files(gen, m) for m in linked}
    hashes = json.loads(Path(a.expected).read_text())['exefs_sha256']
    missing = [n for n in linked if n not in hashes]
    if missing:
        sys.exit(f'{a.expected}: no exefs_sha256 for {missing}')
    write_if_changed(out / 'modules.c', module_c(layout, linked, hashes, a.version, gid))
    count, size, dt = romfs_index.write(Path(a.romfs), out)
    print(f'romfs index: {count} files, {size >> 20} MiB, {dt:.1f} s')
    gen_flags = list(GEN_FLAGS)
    rt_flags = list(RT_FLAGS)
    if a.pgo:
        prof = Path(a.pgo).resolve()
        gen_flags[0] = '-O2'
        pgo = [f'-fprofile-use={q(prof.as_posix())}', '-Wno-profile-instr-unprofiled', '-Wno-profile-instr-out-of-date',
               '-Wno-backend-plugin', '-mllvm', '-static-func-full-module-prefix=false']
        gen_flags += pgo
    inc = f'-isystem {qp(include)} -I {qp(runtime)} -I {qp(gen)} -I {qp(out)}'
    sources = [(f, 'cc_gen', m) for m, files in mods.items() for f in files]
    sources += [(runtime / n, 'cc_rt', 'runtime') for n in INSTALL_SIDE] + [(out / 'modules.c', 'cc_rt', 'runtime')]
    budget = min(build_game.MEM_BUDGET, int(build_game.total_ram() * 0.8))
    jobs = a.jobs or max(1, min(os.cpu_count() or 1, budget // build_game.JOB_MEM))
    objs = []
    text, lib = ninja_text(clang, lld, sources, out, inc, gen_flags, rt_flags, jobs, objs)
    write_if_changed(out / 'build.ninja', text)
    env = dict(os.environ)
    env['PATH'] = str(llvm / 'bin') + os.pathsep + env.get('PATH', '')
    print(f'build_ps5: {sum(len(v) for v in mods.values())} generated files in {len(linked)} modules, {jobs} jobs',
          flush=True)
    code = subprocess.call([str(ninja), '-f', str(out / 'build.ninja'), '-j', str(jobs)], env=env, cwd=out)
    if code:
        sys.exit(code)
    imports = imports_of(nm, lib)
    if exports_file.is_file():
        have = set(exports_file.read_text(encoding='utf-8').split())
        lack = [n for n in imports if n not in have]
        if lack:
            sys.exit(f'build_ps5: the game code imports {len(lack)} names TSword on the PS5 does not export: '
                     f'{" ".join(lack[:20])}')
    (out / 'tsgame.so.id').write_text(gid + '\n', encoding='utf-8', newline='\n')
    print(f'{lib}: {lib.stat().st_size >> 20} MiB, {len(imports)} imports, game code id {gid}')


# ---- --dry-run (TSwordPS5's CI)

STAND_INS = {
    'layout.h': '#pragma once\n#define MOD_MAIN_BASE 0x7000000000ull\n#define MOD_MAIN_SIZE 0x8000000ull\n'
                '#define MOD_COUNT 1\n#define MOD_LIST(X) X(main, MAIN)\n',
    'layout.json': '[{"name": "main", "base": "0x7000000000", "size": "0x8000000"}]\n',
    'patches.h': '#pragma once\n#define RT_PATCH_COUNT 1\n#define RT_PATCH_PAGE 0x7008000000ull\n'
                 '#define RT_PATCH_SETS(X) X(0, dry_run, "TOTK_DRY_RUN", 1)\n',
    'romfs_config.h': '#pragma once\n#define ROMFS_IMAGE_SIZE 1ull\n#define ROMFS_IMAGE_SHA256 "00"\n'
                      '#define ROMFS_INDEX_SHA256 "00"\n#define ROMFS_FILE_COUNT 1ull\n',
    'main_table.c': '#include "cpu.h"\nconst uint64_t main_fn_addrs[1] = {0x7000000000ull};\n'
                    'static void f_7000000000(Cpu *c) { (void)c; }\nconst GuestFn main_fn_ptrs[1] = {f_7000000000};\n'
                    'const uint64_t main_fn_count = 1;\n',
}


def inline_functions(clang, runtime, include, gen):
    """The names of every function defined in cpu.h and what it includes from runtime/ (simd.h, simd_fp.h...)."""
    probe = gen / 'inline_probe.c'
    probe.write_text('#include "cpu.h"\n')
    r = subprocess.run([str(clang), *TARGET_FLAGS, '-isystem', str(include), '-I', str(runtime), '-fsyntax-only',
                        '-Xclang', '-ast-dump=json', str(probe)], capture_output=True, text=True)
    if r.returncode:
        sys.exit(f'build_ps5: the inline probe: {r.stderr[-2000:]}')
    names, cur, rt = [], None, str(runtime)
    for d in json.loads(r.stdout)['inner']:
        loc = d.get('loc', {})
        f = loc.get('file') or (loc.get('expansionLoc') or {}).get('file') or (loc.get('spellingLoc') or {}).get('file')
        if f:
            cur = f
        if (d.get('kind') == 'FunctionDecl' and cur and Path(cur).resolve().is_relative_to(Path(rt).resolve())
                and any(i.get('kind') == 'CompoundStmt' for i in d.get('inner', []))):
            names.append(d['name'])
    return list(dict.fromkeys(names))


def dry_run(a):
    sdk, runtime, deps, out = (Path(x).resolve() for x in (a.sdk, a.runtime, a.deps, a.out))
    clang, lld, nm = sdk / 'bin' / 'clang', Path(a.lld or shutil.which('ld.lld') or 'ld.lld'), sdk / 'bin' / 'llvm-nm'
    sys_inc = sdk / 'target' / 'include'
    shutil.rmtree(out, ignore_errors=True)
    gen = out / 'gen'
    gen.mkdir(parents=True)
    for name, text in STAND_INS.items():
        (gen / name).write_text(text)
    (gen / 'main').mkdir()
    (gen / 'main' / 'manifest.json').write_text('{"chunks": ["main_table.c"]}')
    shutil.move(gen / 'main_table.c', gen / 'main' / 'main_table.c')
    names = inline_functions(clang, runtime, sys_inc, gen)
    (gen / 'main' / 'inlines.c').write_text('#include "cpu.h"\n__attribute__((visibility("default"))) void *tsg_inlines[] = {\n'
                                            + ''.join(f'    (void *){n},\n' for n in names) + '};\n')
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import build_game
    hashes = {'main': '0' * 64}
    write_if_changed(out / 'modules.c', module_c([('main', 0x7000000000, 0x8000000)], ['main'], hashes, 'v143',
                                                 gamecode_id(runtime)))
    inc = ['-isystem', str(sys_inc), '-isystem', str(deps / 'include'), '-I', str(runtime), '-I', str(gen), '-I',
           str(out)]
    objs = []
    for src, flags in [(gen / 'main' / 'main_table.c', GEN_FLAGS), (gen / 'main' / 'inlines.c', GEN_FLAGS),
                       *((runtime / n, RT_FLAGS) for n in INSTALL_SIDE), (out / 'modules.c', RT_FLAGS)]:
        o = out / 'obj' / (src.stem + '.o')
        o.parent.mkdir(parents=True, exist_ok=True)
        r = run([clang, *TARGET_FLAGS, *flags, *inc, '-MD', '-MF', f'{o}.d', '-c', src, '-o', o])
        if r.returncode:
            sys.exit(f'build_ps5: {src.name} did not compile for the PS5')
        objs.append(o)
    lib = out / 'tsgame.so'
    if run([lld, *LINK_FLAGS, *objs, '-o', lib]).returncode:
        sys.exit('build_ps5: the dry-run library did not link')
    need = imports_of(nm, lib)
    (out / 'need.txt').write_text('\n'.join(need) + '\n')
    # the headers they read outside runtime/ and the generated ones: the kit's include/
    headers = set()
    for d in out.glob('obj/*.o.d'):
        for tok in re.split(r'\s+', d.read_text().replace('\\\n', ' ')):
            tok = tok.rstrip(':')
            if tok.endswith('.h'):
                headers.add(Path(tok).resolve())
    kit = out / 'include'
    copied = 0
    for h in sorted(headers):
        for base in (sys_inc, deps / 'include'):
            if h.is_relative_to(base):
                dst = kit / h.relative_to(base)
                dst.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(h, dst)
                copied += 1
    (out / 'gamecode.id').write_text(gamecode_id(runtime) + '\n')
    print(f'dry run: {len(names)} inline functions, {len(need)} imports ({" ".join(need)}), {copied} headers into {kit}')


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--id-only', action='store_true')
    ap.add_argument('--dry-run', action='store_true')
    ap.add_argument('--runtime', required=True)
    ap.add_argument('--gen')
    ap.add_argument('--out')
    ap.add_argument('--toolchain')
    ap.add_argument('--ps5', help='the TSword PS5 kit: include/, exports.txt, gamecode.id')
    ap.add_argument('--version', default='v143')
    ap.add_argument('--expected')
    ap.add_argument('--romfs')
    ap.add_argument('--ninja')
    ap.add_argument('--jobs', type=int)
    ap.add_argument('--pgo')
    ap.add_argument('--sdk')
    ap.add_argument('--deps')
    ap.add_argument('--lld')
    a = ap.parse_args()
    if a.id_only:
        print(gamecode_id(a.runtime))
    elif a.dry_run:
        dry_run(a)
    else:
        for k in ('gen', 'out', 'toolchain', 'ps5', 'expected', 'romfs'):
            if not getattr(a, k):
                sys.exit(f'build_ps5: --{k} is needed')
        build(a)


if __name__ == '__main__':
    main()
