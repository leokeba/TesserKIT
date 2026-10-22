#!/usr/bin/env python3
"""TesserKIT's version files: library.json, library.properties, idf_component.yml.

  python3 scripts/version.py 0.2.3   writes the version into all three
  python3 scripts/version.py         checks that they agree, prints it

The release workflow stamps them (firmware.yml's stamp-command) in the
commit its tag points at, so a tag's files state its version; main's keep the
series they were last raised to, and aren't edited by hand for releases
(docs/DESIGN.md section 13).
"""
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SEMVER = re.compile(r'^[0-9]+\.[0-9]+\.[0-9]+$')

# Each file's version line, and how to write it. A line, not a parser: the
# files keep their formatting and comments.
# library.json's dependencies are on lines of their own ({ "name": ...,
# "version": "<url>" }), which the package's pattern doesn't match.
FILES = {
    'library.json': re.compile(r'^(\s*"version":\s*")([^"]*)(",?\s*)$', re.M),
    'library.properties': re.compile(r'^(version=)(.*?)(\s*)$', re.M),
    'idf_component.yml': re.compile(r'^(version:\s*")([^"]*)("\s*)$', re.M),
}


def read(name):
    with open(os.path.join(ROOT, name), encoding='utf-8') as f:
        return f.read()


def found(name):
    text = read(name)
    pattern = FILES[name]
    m = pattern.search(text)
    if not m:
        sys.exit(f'{name}: no version line')
    return pattern, text, m.group(2)


def main():
    if len(sys.argv) > 2:
        sys.exit(__doc__)
    if len(sys.argv) == 1:
        versions = {name: found(name)[2] for name in FILES}
        if len(set(versions.values())) != 1:
            sys.exit('the version files disagree: ' + ', '.join(f'{n} {v}' for n, v in versions.items()))
        print(next(iter(versions.values())))
        return
    version = sys.argv[1]
    if not SEMVER.match(version):
        sys.exit(f'not X.Y.Z: {version}')
    for name in FILES:
        pattern, text, _ = found(name)
        text = pattern.sub(lambda m: m.group(1) + version + m.group(3), text, count=1)
        with open(os.path.join(ROOT, name), 'w', encoding='utf-8') as f:
            f.write(text)
    json.loads(read('library.json'))  # still JSON
    print(version)


if __name__ == '__main__':
    main()
