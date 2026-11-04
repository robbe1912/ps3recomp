"""Exercise production registry/header admission and real historical controls."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]


def function(text, name):
    start = text.index(name)
    opening = text.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


def fragments(text):
    registry = text[text.index('typedef void (*spu_fn)(spu_context*);'):
                    text.index('/* ---- Swappable code overlays')]
    types = text[text.index('typedef struct { uint32_t src_ea;'):
                 text.index('void spu_overlay_register_source(')]
    return registry, types + function(text, 'void spu_overlay_register_sig(') + '\n' + function(text, 'int spu_overlay_match_sig(')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT/'runtime/spu/spu_channels.c')
    parser.add_argument('--fixture', type=Path, default=ROOT/'runtime/spu/tests/test_spu_policy_overlay.c')
    parser.add_argument('--historical-source', type=Path)
    args = parser.parse_args()
    source, fixture = args.source.resolve(), args.fixture.resolve()
    legacy = fixture.with_name('spu_policy_overlay_legacy.inc')
    text = source.read_text(encoding='utf-8')
    registry, overlays = fragments(text)
    old_function, old_match = legacy.read_text(encoding='utf-8').split('/* LEGACY_MATCH */')
    old_function = old_function[old_function.index('spu_lifted_fn spu_lifted_lookup('):].strip()
    current_function = function(registry, 'spu_lifted_fn spu_lifted_lookup(')
    cases = [('production', registry, overlays, None),
             ('legacy', registry.replace(current_function, old_function), overlays, old_match)]
    if args.historical_source:
        historical = args.historical_source.read_text(encoding='utf-8')
        historical_registry, historical_overlays = fragments(historical)
        a = historical.index('    /* WWS job-code overlay match:')
        b = historical.index('    /* WWS job-code fallback:', a)
        historical_match = historical[a:b]
        assert function(historical_registry, 'spu_lifted_fn spu_lifted_lookup(').strip() == old_function
        assert historical_match.strip() == old_match.strip()
        cases.append(('historical-source', historical_registry, historical_overlays, historical_match))
    compiler = os.environ.get('MV_CLANG') or os.environ.get('CC') or shutil.which('clang-cl' if os.name == 'nt' else 'cc')
    if not compiler:
        parser.error('set MV_CLANG/CC or install a C compiler')
    out = ROOT/'temp/checks'/('policy-overlay-'+str(time.time_ns()))
    out.mkdir(parents=True)
    result = {'source': str(source), 'hashes': {}, 'cases': []}
    for p in [source, source.parent/'spu_policy_overlay.h', source.parent/'spu_context.h', fixture, legacy, Path(__file__)]:
        result['hashes'][str(p)] = hashlib.sha256(p.read_bytes()).hexdigest()
    for label, body, overlay_body, match in cases:
        folder = out/label
        folder.mkdir()
        if match is None:
            wrapper = 'spu_policy_overlay_refresh(ctx, pc); return spu_lifted_lookup(ctx, pc);'
        else:
            wrapper = 'spu_fn fn = spu_lifted_lookup(ctx, pc);\n' + match + '\nreturn fn;'
        include = '#include "spu_policy_overlay.h"\n' + body + '\n' + overlay_body
        include += '\nstatic spu_fn resolve_for_test(spu_context* ctx, uint32_t pc) { ctx->pc=pc;\n' + wrapper + '\n}\n'
        (folder/'policy_overlay_production.inc').write_text(include, encoding='utf-8')
        exe = folder/('fixture.exe' if os.name == 'nt' else 'fixture')
        includes = [source.parent, source.parents[2]/'include', folder]
        if os.name == 'nt':
            command = [compiler, '/nologo', '/TC', '/std:c17', '/O2', '/UNDEBUG', '/D_CRT_SECURE_NO_WARNINGS']
            command += ['/I'+str(p) for p in includes] + [str(fixture), '/Fo'+str(folder/'fixture.obj'), '/Fe'+str(exe)]
        else:
            command = [compiler, '-std=c11', '-O2', '-UNDEBUG', '-Wall', '-Wextra', '-Werror']
            command += ['-I'+str(p) for p in includes] + [str(fixture), '-o', str(exe)]
        built = subprocess.run(command, capture_output=True, text=True, timeout=120)
        (folder/'build.log').write_text(built.stdout+built.stderr, encoding='utf-8')
        assert built.returncode == 0, str(folder/'build.log')
        ran = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
        (folder/'run.log').write_text(ran.stdout+ran.stderr, encoding='utf-8')
        record = dict(label=label, command=command, returncode=ran.returncode, stdout=ran.stdout, stderr=ran.stderr)
        result['cases'].append(record)
        if label == 'production':
            assert ran.returncode == 0 and re.search(r'POLICY_OVERLAY checks=\d+ failures=0', ran.stdout), record
        else:
            assert ran.returncode != 0 and 'POLICY_OVERLAY checks=' in ran.stdout, record
            assert 'FAIL policy image3 ignores stale primary' in ran.stderr, record
            assert 'FAIL unknown header retires stale overlay' in ran.stderr, record
        print(label+': '+ran.stdout.strip())
    (out/'result.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    print('PASS production admission and real historical controls; evidence '+str(out))


if __name__ == '__main__':
    main()
