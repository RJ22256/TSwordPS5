"""Code recon of the extracted NSO modules (analysis ELFs from MacNativeGames' prepare_modules.py).

Per module: segment sizes, function count from .eh_frame_hdr (via MOD0), and an instruction histogram
(capstone, no detail) grouped into classes that matter for ARM64 -> x86-64 translation.

usage: python tools/recon_code.py work/extracted/v143/elf --output work/analysis/v143-code.json
"""
import argparse
import collections
import json
import multiprocessing
import pathlib
import struct

import capstone

CHUNK = 1 << 20  # bytes of .text per worker task

ATOMIC_EXCL = {'ldxr', 'ldxrb', 'ldxrh', 'ldaxr', 'ldaxrb', 'ldaxrh', 'stxr', 'stxrb', 'stxrh', 'stlxr', 'stlxrb',
               'stlxrh', 'ldxp', 'ldaxp', 'stxp', 'stlxp', 'clrex'}
LSE_PREFIX = ('cas', 'swp', 'ldadd', 'ldclr', 'ldeor', 'ldset', 'ldsmax', 'ldsmin', 'ldumax', 'ldumin',
              'stadd', 'stclr', 'steor', 'stset', 'stsmax', 'stsmin', 'stumax', 'stumin')
BARRIERS = {'dmb', 'dsb', 'isb'}
ACQREL = {'ldar', 'ldarb', 'ldarh', 'stlr', 'stlrb', 'stlrh', 'ldapr', 'ldaprb', 'ldaprh'}


def classify(mnemonic, ops):
    m = mnemonic.split('.')[0]
    if m in ('svc', 'hvc', 'brk', 'udf', 'hlt'):
        return 'system:' + m
    if m in ('mrs', 'msr'):
        reg = ops.split(',')[1 if m == 'mrs' else 0].strip().lower()
        return f'system:{m} {reg}'
    if m in ATOMIC_EXCL:
        return 'atomic:exclusive'
    if m.startswith(LSE_PREFIX):
        return 'atomic:lse'
    if m in BARRIERS:
        return 'barrier'
    if m in ACQREL:
        return 'atomic:acq_rel'
    if m in ('br', 'braa', 'brab', 'braaz', 'brabz'):
        return 'branch:indirect_br'
    if m in ('blr', 'blraa', 'blrab', 'blraaz', 'blrabz'):
        return 'branch:indirect_call'
    if m in ('ret', 'retaa', 'retab'):
        return 'branch:ret'
    if m == 'bl':
        return 'branch:call'
    if m in ('b', 'cbz', 'cbnz', 'tbz', 'tbnz') or mnemonic.startswith('b.'):
        return 'branch:direct'
    vector = ' v' in ' ' + ops.replace('{', ' ').replace('[', ' ') and any(
        t.strip().startswith(('v', '{v')) for t in ops.split(','))
    if vector:
        return 'simd:vector'
    regs = [t.strip() for t in ops.split(',')]
    if any(r[:1] in ('s', 'd', 'h', 'q', 'b') and r[1:].isdigit() for r in regs if r):
        if m.startswith(('ld', 'st')):
            return 'fp:load_store'
        return 'fp:scalar'
    if m.startswith(('ld', 'st', 'prfm')):
        return 'int:load_store'
    if m == 'nop':
        return 'nop'
    return 'int:alu'


def scan(args):
    data, base = args
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    md.skipdata = True
    classes = collections.Counter()
    mnemonics = collections.Counter()
    x18 = 0
    for _, _, mnemonic, ops in md.disasm_lite(data, base):
        if mnemonic == '.byte':
            classes['data_in_text'] += 1
            continue
        mnemonics[mnemonic] += 1
        classes[classify(mnemonic, ops)] += 1
        if 'x18' in ops or 'w18' in ops:
            x18 += 1
    return classes, mnemonics, x18


def eh_frame_fdes(elf, segments):
    """Function count from the binary-search table of .eh_frame_hdr (MOD0 points to it)."""
    text = segments['text']
    mod0_ptr = struct.unpack_from('<I', elf, text['file'] + 4)[0]
    if elf[text['file'] + mod0_ptr:text['file'] + mod0_ptr + 4] != b'MOD0':
        return None
    mod0 = mod0_ptr
    _, dyn, bss0, bss1, eh0, eh1, obj = struct.unpack_from('<4s6i', elf, text['file'] + mod0)
    eh_hdr = mod0 + eh0
    off = file_offset(eh_hdr, segments)
    version, ptr_enc, count_enc, table_enc = elf[off:off + 4]
    if version != 1 or count_enc != 0x03 or table_enc != 0x3b:
        return dict(eh_frame_hdr=hex(eh_hdr), note='unexpected encodings')
    count = struct.unpack_from('<I', elf, off + 8)[0]
    starts = [eh_hdr + struct.unpack_from('<i', elf, off + 12 + 8 * i)[0] for i in range(count)]
    return dict(eh_frame_hdr=hex(eh_hdr), fde_count=count, starts=starts)


def file_offset(vaddr, segments):
    for s in segments.values():
        if s['addr'] <= vaddr < s['addr'] + s['size']:
            return s['file'] + vaddr - s['addr']
    raise ValueError(hex(vaddr))


def load_segments(elf):
    phoff, = struct.unpack_from('<Q', elf, 0x20)
    phentsize, phnum = struct.unpack_from('<HH', elf, 0x36)
    loads = []
    for i in range(phnum):
        p_type, flags, off, vaddr, _, filesz, memsz, _ = struct.unpack_from('<IIQQQQQQ', elf, phoff + i * phentsize)
        if p_type == 1:
            loads.append(dict(file=off, addr=vaddr, size=filesz, memsz=memsz))
    return dict(zip(['text', 'rodata', 'data'], loads))


def main():
    p = argparse.ArgumentParser()
    p.add_argument('elfdir', type=pathlib.Path)
    p.add_argument('--output', type=pathlib.Path, required=True)
    a = p.parse_args()
    report = {}
    with multiprocessing.Pool() as pool:
        for module in ('main', 'sdk', 'subsdk0'):
            elf = (a.elfdir / f'{module}.elf').read_bytes()
            seg = load_segments(elf)
            t = seg['text']
            text = elf[t['file']:t['file'] + t['size']]
            fdes = eh_frame_fdes(elf, seg)
            starts = fdes.pop('starts', []) if fdes else []
            sizes = sorted(b - a_ for a_, b in zip(starts, starts[1:]) if b > a_)
            tasks = [(text[i:i + CHUNK], t['addr'] + i) for i in range(0, len(text), CHUNK)]
            classes, mnemonics, x18 = collections.Counter(), collections.Counter(), 0
            for c, m, x in pool.imap_unordered(scan, tasks):
                classes.update(c); mnemonics.update(m); x18 += x
            total = sum(v for k, v in classes.items() if k != 'data_in_text')
            report[module] = dict(
                segments={k: dict(addr=hex(v['addr']), size=v['size'], memsz=v['memsz']) for k, v in seg.items()},
                instructions=total, eh_frame=fdes,
                function_size_bytes=dict(median=sizes[len(sizes) // 2], p90=sizes[int(len(sizes) * .9)],
                                         max=sizes[-1]) if sizes else None,
                classes={k: dict(count=v, share=round(v / total, 4)) for k, v in classes.most_common()},
                x18_operand_instructions=x18,
                top_mnemonics=mnemonics.most_common(60))
            print(module, total, 'insns', fdes and fdes.get('fde_count'), 'FDEs', flush=True)
    a.output.parent.mkdir(parents=True, exist_ok=True)
    a.output.write_text(json.dumps(report, indent=1))


if __name__ == '__main__':
    main()
