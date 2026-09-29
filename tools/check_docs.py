#!/usr/bin/env python3
"""Check local Markdown links, heading anchors and fenced-code balance offline."""
from collections import Counter
from pathlib import Path
import re
from urllib.parse import unquote, urlsplit

ROOT = Path(__file__).resolve().parents[1]


def prose(path):
    lines, fence = [], None
    for line in path.read_text().splitlines():
        marker = re.match(r'^\s{0,3}(`{3,}|~{3,})', line)
        if marker:
            token = marker.group(1)
            suffix = line[marker.end():]
            if fence is None:
                # Backticks in an info string make this a code span, not a
                # fenced block (e.g. the historical ```make bsgsd``` line).
                if token[0] == '`' and '`' in suffix:
                    lines.append(line)
                    continue
                fence = token
            elif token[0] == fence[0] and len(token) >= len(fence) and not suffix.strip():
                fence = None
            continue
        if fence is None:
            lines.append(line)
    if fence:
        raise ValueError(f'{path.relative_to(ROOT)}: unclosed code fence')
    return '\n'.join(lines)


def anchors(path):
    body = prose(path)
    result = set(re.findall(r'<a\s+id="([^"]+)"', body))
    seen = Counter()
    for heading in re.findall(r'^#{1,6}\s+(.+)', body, re.M):
        heading = re.sub(r'\[([^]]+)\]\([^)]*\)', r'\1', heading)
        slug = re.sub(r'[^\w\- ]', '', heading.lower()).replace(' ', '-')
        count = seen[slug]
        seen[slug] += 1
        result.add(slug + (f'-{count}' if count else ''))
    return result


def main():
    files = [*ROOT.glob('*.md'), *ROOT.glob('docs/**/*.md'), *ROOT.glob('kernels/**/*.md')]
    errors = []
    for path in sorted(files):
        try:
            body = prose(path)
            links = re.findall(r'\]\(([^\s)]+)\)', body)
            definitions = dict(re.findall(r'^\[([^]]+)\]:\s+(\S+)', body, re.M))
            for label in re.findall(r'\]\[([^]]+)\]', body):
                if label not in definitions:
                    errors.append(f'{path.relative_to(ROOT)}: undefined reference [{label}]')
            links += list(definitions.values())
            for link in links:
                parsed = urlsplit(link)
                if parsed.scheme or parsed.netloc:
                    continue
                target = (path.parent / unquote(parsed.path)).resolve() if parsed.path else path
                if not target.exists():
                    errors.append(f'{path.relative_to(ROOT)}: missing {link}')
                elif parsed.fragment and target.suffix == '.md' and unquote(parsed.fragment) not in anchors(target):
                    errors.append(f'{path.relative_to(ROOT)}: missing anchor in {link}')
        except ValueError as error:
            errors.append(str(error))
    if errors:
        print('\n'.join(errors))
        return 1
    print(f'Checked local links, anchors and code fences in {len(files)} Markdown files')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
