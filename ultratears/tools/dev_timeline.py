"""Read a TOTK_TIMELINE file (src/gfx/nvn/totk_timeline_win.c) and show who waits for whom in each frame.

usage: python tools/dev_timeline.py <file.tsv> [frame]
Without a frame: per frame, the time of each kind of span (game thread submit / present / sync waits, the replay of
submissions on the translation thread, GPU spans). With a frame: that frame's events in order, relative to its first.
"""
import collections
import sys

SPANS = {'sub_in': ('sub_out', 'submit'), 'pres_in': ('pres_out', 'present'), 'wait_in': ('wait_out', 'sync wait'),
         'acq_in': ('acq_out', 'acquire'), 'rep_begin': ('rep_end', 'replay'), 'finish_in': ('finish_out', 'finish')}


def load(path):
    ev = []
    for line in open(path, encoding='utf-8'):
        if line.startswith('#'):
            continue
        t, kind, tid, frame, a, b = line.rstrip('\n').split('\t')
        ev.append((int(t), kind, int(tid), int(frame), int(a), int(b)))
    ev.sort()
    return ev


def spans(ev):
    """(start, end, label, tid, frame) for every paired span and every GPU span."""
    out, open_ = [], {}
    for t, kind, tid, frame, a, b in ev:
        if kind in SPANS:
            open_[(tid, kind)] = (t, frame)
        elif kind == 'gpu':
            out.append((t, b, 'GPU', 0, frame))
        else:
            for start, (end, label) in SPANS.items():
                if kind == end and (tid, start) in open_:
                    t0, f0 = open_.pop((tid, start))
                    out.append((t0, t, label, tid, f0))
    return out


def main():
    ev = load(sys.argv[1])
    sp = spans(ev)
    pres = [t for t, k, *_ in ev if k == 'pres_in']
    if len(sys.argv) > 2:
        f = int(sys.argv[2])
        t0 = min(t for t, k, tid, fr, a, b in ev if fr == f)
        for s, e, label, tid, fr in sorted(sp):
            if fr == f or (label == 'GPU' and t0 - 50e6 <= s <= t0 + 150e6):
                print(f'{(s - t0) / 1e6:8.2f} .. {(e - t0) / 1e6:8.2f} ms  {e - s and (e - s) / 1e6:7.2f} ms  {label:10} thread {tid}')
        return
    per = collections.defaultdict(lambda: collections.defaultdict(float))
    threads = collections.defaultdict(lambda: collections.defaultdict(float))
    for s, e, label, tid, fr in sp:
        per[fr][label] += (e - s) / 1e6
        threads[label][tid] += (e - s) / 1e6
    frames = sorted(f for f in per if f)
    gaps = [(b - a) / 1e6 for a, b in zip(pres, pres[1:])]
    print(f'presents: {len(pres)}, frame time (present to present) mean {sum(gaps) / max(1, len(gaps)):.1f} ms')
    labels = ['submit', 'present', 'acquire', 'sync wait', 'finish', 'replay', 'GPU']
    print('frame  ' + ''.join(f'{l:>11}' for l in labels))
    for f in frames:
        print(f'{f:5}  ' + ''.join(f'{per[f].get(l, 0):11.1f}' for l in labels))
    print('threads per span kind:', {k: {t: round(v, 1) for t, v in d.items()} for k, d in threads.items()})


if __name__ == '__main__':
    main()
