"""What of the repository is public: the GitHub branch `public` and the builder package carry the files git tracks,
except PRIVATE (the internal working notes, not published for now: the user's decision of 2026-10-25).

usage: python tools/public_tree.py --publish   (moves the branch `public` to a new commit named after the tag v<x.y.z> on
                                            HEAD; pushing stays a separate, explicit step: git push origin public:main)
The local history (master) keeps everything; `public` is one commit per release on top of the previous one, so only
what a release holds reaches GitHub.
"""
import argparse
import os
import pathlib
import subprocess
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import version  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parent.parent
PRIVATE = ['docs']


def git(*args, env=None):
    return subprocess.run(['git', '-C', str(ROOT)] + list(args), check=True, capture_output=True, text=True,
                          env=env).stdout.strip()


def tree(ref='HEAD'):
    """The id of the public tree of ref: its tree without PRIVATE."""
    with tempfile.TemporaryDirectory() as d:
        env = dict(os.environ, GIT_INDEX_FILE=str(pathlib.Path(d) / 'index'))
        git('read-tree', ref, env=env)
        git('rm', '-r', '-q', '--cached', '--ignore-unmatch', '--', *PRIVATE, env=env)
        return git('write-tree', env=env)


def publish(version):
    t = tree()
    parent = subprocess.run(['git', '-C', str(ROOT), 'rev-parse', '-q', '--verify', 'refs/heads/public'],
                            capture_output=True, text=True).stdout.strip()
    if parent and git('rev-parse', parent + '^{tree}') == t:
        return parent   # nothing changed since the last release
    msg = (f'UltraTears {version}\n\nAn unofficial native PC port of The Legend of Zelda: Tears of the Kingdom (Switch 1.4.3)'
           ' by static recompilation, built on the player\'s PC from their own dump. See README.md.\n\n'
           'Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>')
    commit = git('commit-tree', t, *(['-p', parent] if parent else []), '-m', msg)
    git('branch', '-f', 'public', commit)
    return commit


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--publish', action='store_true', required=True)
    a = ap.parse_args()
    if git('status', '--porcelain', '--untracked-files=no'):
        raise SystemExit('commit first: the public branch is made from HEAD')
    v = version.release()
    if not v:
        raise SystemExit('HEAD has no release tag: git tag v<x.y.z>')
    print(f'public {v} -> {publish(v)}')


if __name__ == '__main__':
    main()
