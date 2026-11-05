"""Shared by the SPU CPU harnesses: the actual owned-job option readers.

spu_interp.c and the scoped drain body call spu_native_owned_enabled() and
spu_native_owned_observed() (through spu_native_owned.h). Those readers live in
spu_channels.c beside host services the harnesses do not link, so they are
extracted verbatim here instead of being replaced by permissive stubs: the
fixtures read SPU_NATIVE_OWNED_JOBS and the observation options exactly as the
runtime does, and reject an invalid value the same way."""
from pathlib import Path
import re

READERS = ('int spu_native_owned_enabled(void)', 'int spu_native_owned_observed(void)')


def function(text, signature):
    """Exact text of one top-level C function, braces matched outside comments/strings."""
    assert text.count(signature) == 1, signature
    start = text.index(signature)
    opening = text.index('{', start)
    clean = re.sub(r'/\*[\s\S]*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
                   lambda m: ' ' * len(m[0]), text)
    depth = 1
    i = opening + 1
    while depth:
        depth += (clean[i] == '{') - (clean[i] == '}')
        i += 1
    return text[start:i]


def source(root):
    """C translation unit holding the production readers from spu_channels.c."""
    channels = (Path(root) / 'runtime/spu/spu_channels.c').read_text()
    body = '\n'.join(function(channels, s) for s in READERS)
    return ('/* Extracted verbatim from runtime/spu/spu_channels.c by tools/spu_owned_options.py. */\n'
            '#include "spu_native_owned.h"\n'
            '#include "ps3emu/native_atomic_observer.h"\n'
            '#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n\n' + body + '\n')
