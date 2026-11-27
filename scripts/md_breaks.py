#!/usr/bin/env python3
"""Puts each sentence of a Markdown file on its own line.

    scripts/md_breaks.py FILE...           rewrite the files in place
    scripts/md_breaks.py --check FILE...   exit 1 if a file would change

Paragraph-long lines defeat line-based tools: `grep -n` prints a whole
paragraph per hit (30 KB for one `grep | head` on AGENTS.md), a reader that
truncates long lines loses their end, and an offset/limit read can't take a
small piece. One sentence per line renders the same (a line break inside a
paragraph is a space) and gives diffs that show the sentence that changed.

Left alone: fenced code, tables, headings, HTML, and anything the splitter
can't place safely (inside a code span, after "e.g.", before a digit or a
list marker). A list item's or quote's continuation lines are indented to
its content, so they stay inside it.
"""
import re
import sys

ITEM = re.compile(r'^(\s*)([-*+]|\d+[.)])(\s+)')
QUOTE = re.compile(r'^(\s*(?:>\s?)+)')
# A sentence end: . ! or ? (maybe closed by ) * " ' or `), one space, then
# a capital, a backtick, an opening parenthesis, emphasis or a link.
END = re.compile(r'(?<=[.!?])([)*"\'’]*) (?=[A-Z`(\[]|\*\*?[A-Za-z`])')
ABBREV = ('e.g.', 'i.e.', 'vs.', 'cf.', 'approx.', 'Fig.', 'No.')


def code_spans(text):
    """Ranges of `code` spans, where no split happens."""
    spans, i = [], 0
    while True:
        m = re.compile(r'`+').search(text, i)
        if not m:
            return spans
        close = text.find(m.group(0), m.end())
        if close < 0:
            return spans
        spans.append((m.start(), close + len(m.group(0))))
        i = close + len(m.group(0))


def split_sentences(body):
    spans = code_spans(body)
    pieces, start = [], 0
    for m in END.finditer(body):
        cut = m.start() + len(m.group(1))  # after the closing marks
        if any(a <= cut < b for a, b in spans):
            continue
        before = body[start:cut]
        if before.endswith(ABBREV) or len(before.strip()) < 3:
            continue
        pieces.append(before)
        start = cut + 1  # skip the space
    pieces.append(body[start:])
    return pieces


def rewrite(lines):
    out, fence = [], None
    for line in lines:
        stripped = line.lstrip()
        f = re.match(r'(`{3,}|~{3,})', stripped)
        if fence:
            out.append(line)
            if f and stripped.startswith(fence):
                fence = None
            continue
        if f:
            fence = f.group(1)
            out.append(line)
            continue
        if (not stripped or stripped[0] in '#|<' or line.startswith('    ') and not ITEM.match(line)
                and not out_in_list(out)):
            out.append(line)
            continue
        m = ITEM.match(line)
        q = QUOTE.match(line)
        if m:
            prefix = line[:m.end()]
            indent = ' ' * len(prefix)
        elif q:
            prefix = q.group(1)
            indent = prefix
        else:
            prefix = line[:len(line) - len(stripped)]
            indent = prefix
        body = line[len(prefix):].rstrip('\n')
        hard = body.endswith('  ')
        pieces = split_sentences(body.rstrip() if not hard else body)
        if len(pieces) == 1:
            out.append(line)
            continue
        out.append(prefix + pieces[0] + '\n')
        out.extend(indent + p + '\n' for p in pieces[1:])
    return out


def out_in_list(out):
    """Whether the last non-blank line written belongs to a list item, so an
    indented line continues it rather than starting a code block."""
    for prev in reversed(out):
        if prev.strip():
            return bool(ITEM.match(prev)) or prev.startswith('  ')
    return False


def main(argv):
    check = '--check' in argv
    files = [a for a in argv if a != '--check']
    changed = []
    for path in files:
        with open(path, encoding='utf-8') as f:
            lines = f.readlines()
        new = rewrite(lines)
        if new != lines:
            changed.append(path)
            if not check:
                with open(path, 'w', encoding='utf-8') as f:
                    f.writelines(new)
    for path in changed:
        print(('would change ' if check else 'rewrote ') + path)
    return 1 if check and changed else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
