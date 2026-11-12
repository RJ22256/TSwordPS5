"""Development: the expression tree of a variable in SPIRV-Cross GLSL output (which inputs feed an output).

usage: python tools/dev_glsl_tree.py <file.glsl> <variable, e.g. _64.x> [depth]
Follows `float _N = expr;` / `_64.x = _N;` assignments upward; uniforms (fp_cN_1._m0[i].c), texture samples and
inputs are leaves.
"""
import re
import sys


def main():
    path, root = sys.argv[1], sys.argv[2]
    depth = int(sys.argv[3]) if len(sys.argv) > 3 else 12
    defs = {}
    for line in open(path, encoding="utf-8"):
        m = re.match(r"\s*(?:precise\s+)?(?:float|int|uint|bool|vec[234]|ivec[234])?\s*([_\w.]+)\s*=\s*(.+);\s*$", line)
        if m:
            defs.setdefault(m.group(1), []).append(m.group(2))
    seen = set()

    def show(var, d, indent):
        exprs = defs.get(var)
        if not exprs:
            print(f"{indent}{var}  (input)")
            return
        expr = exprs[-1] if len(exprs) == 1 else " | ".join(exprs)
        print(f"{indent}{var} = {expr}")
        if d == 0 or var in seen:
            return
        seen.add(var)
        for ref in sorted(set(re.findall(r"\b_\d+\b(?:\.[xyzw])?", expr))):
            if ref != var:
                show(ref, d - 1, indent + "  ")

    show(root, depth, "")


if __name__ == "__main__":
    main()
