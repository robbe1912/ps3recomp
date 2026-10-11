#!/usr/bin/env python3
"""Account for base-relative pointers in private, read-only memory snapshots.

The audit is format independent. The optional plan builder supports only Havok
packfile v4, big-endian, 32-bit pointers. It does not execute guest code, repair
memory, infer guest PCs, or claim a writer from a post-operation snapshot.
"""
import argparse
import hashlib
import json
import os
import struct
import sys
from pathlib import Path


class AuditError(ValueError):
    pass


def require(condition, message):
    if not condition:
        raise AuditError(message)


def mapping(value, message):
    require(isinstance(value, dict), message)


def protect_inputs(output, inputs):
    """Also detect hard links, which resolving paths alone cannot protect."""
    for path in inputs:
        require(output.resolve() != path.resolve() and
                not (output.exists() and path.exists() and os.path.samefile(output, path)),
                'Output would overwrite an input')


def digest(data):
    return hashlib.sha256(data).hexdigest()


def integer(value):
    require(not isinstance(value, bool), 'Boolean is not an address')
    try:
        result = int(value, 0) if isinstance(value, str) else int(value)
    except (ValueError, TypeError) as exc:
        raise AuditError('Expected integer or 0x-prefixed address') from exc
    require(isinstance(value, (str, int)) and result >= 0, 'Expected unsigned integer')
    return result


def havok_v4_plan(raw):
    require(len(raw) >= 64, 'Truncated packfile header')
    def word(offset):
        require(offset + 4 <= len(raw), 'Word outside raw input')
        return struct.unpack_from('>I', raw, offset)[0]
    require(raw[:8] == bytes.fromhex('57e0e05710c0c010'), 'Wrong packfile magic')
    require(word(12) == 4 and raw[16:20] == bytes([4, 0, 1, 1]),
            'Only v4 big-endian 32-bit layout [4,0,1,1] is supported')
    count = word(20)
    require(0 < count <= (len(raw) - 64) // 48, 'Invalid section count')
    header_end = 64 + 48 * count
    sections = []
    for index in range(count):
        at = 64 + 48 * index
        start, *bounds = struct.unpack_from('>7I', raw, at + 20)
        require(bounds == sorted(bounds) and start >= header_end and
                start + bounds[-1] <= len(raw), 'Invalid section bounds')
        tag = raw[at:at + 19].split(b'\0', 1)[0].decode('ascii', errors='strict')
        sections.append({'name': tag, 'start': start, 'bounds': bounds})
    spans = sorted((s['start'], s['start'] + s['bounds'][-1]) for s in sections)
    require(all(a[1] <= b[0] for a, b in zip(spans, spans[1:])), 'Overlapping sections')
    fields, finishing, seen = [], [], set()
    for index, section in enumerate(sections):
        start = section['start']
        local, glob, virtual, exports, imports, end = section['bounds']
        for kind, lo, hi, stride in (('local', local, glob, 8),
                                     ('global', glob, virtual, 12),
                                     ('finish', virtual, exports, 12)):
            # Tables may end with alignment padding rather than a full record.
            table = raw[start + lo:start + hi]
            full = len(table) // stride * stride
            require(all(b == 255 for b in table[full:]), 'Invalid table padding')
            for rel in range(0, full, stride):
                at = start + lo + rel
                source = word(at)
                if source == 0xffffffff:
                    require(raw[at:at + stride] == b'\xff' * stride, 'Invalid sentinel record')
                    continue
                target_section, target = ((index, word(at + 4)) if kind == 'local'
                                          else (word(at + 4), word(at + 8)))
                require(source + 4 <= local and target_section < count and
                        target <= sections[target_section]['bounds'][0], 'Invalid fixup record')
                field = {'offset': start + source,
                         'target_offset': sections[target_section]['start'] + target,
                         'label': f'{section["name"]}:{kind}:{source:x}', 'kind': kind}
                if kind == 'finish':
                    require(target < sections[target_section]['bounds'][0],
                            'Finish classname outside section data')
                    finishing.append({'object_offset': field['offset'],
                                      'class_name_offset': field['target_offset']})
                else:
                    require(field['offset'] not in seen, 'Duplicate pointer source')
                    seen.add(field['offset'])
                    fields.append(field)
    invariants = [{'offset': 0, 'size': header_end, 'sha256': digest(raw[:header_end])}]
    for section in sections:
        lo = section['start'] + section['bounds'][0]
        hi = section['start'] + section['bounds'][-1]
        invariants.append({'offset': lo, 'size': hi - lo, 'sha256': digest(raw[lo:hi])})
    return {'schema': 1, 'pointer_bytes': 4, 'endian': 'big', 'size': len(raw),
            'raw_sha256': digest(raw), 'fields': fields, 'invariants': invariants,
            'finish_records': finishing,
            'limits': 'Local/global relocations only. Finishing records are inventory; '
                      'vtable/registry/callee semantics require independent evidence.'}


def validate_plan(plan):
    mapping(plan, 'Plan must be an object')
    require(type(plan.get('schema')) is int and plan['schema'] == 1, 'Unsupported plan schema')
    width = integer(plan.get('pointer_bytes'))
    require(width in (4, 8) and plan.get('endian') in ('big', 'little'), 'Invalid pointer layout')
    size = integer(plan.get('size'))
    require(size > 0 and isinstance(plan.get('fields'), list) and plan['fields'], 'Empty plan')
    seen = set()
    fields = []
    for field in plan['fields']:
        mapping(field, 'Pointer field must be an object')
        offset = integer(field.get('offset'))
        require(offset + width <= size, 'Pointer outside snapshot')
        require(all(byte not in seen for byte in range(offset, offset + width)),
                'Overlapping pointer fields')
        seen.update(range(offset, offset + width))
        require(('target_offset' in field) != ('expected' in field),
                'Specify exactly one of target_offset or expected')
        key = 'target_offset' if 'target_offset' in field else 'expected'
        value = integer(field[key])
        require(key != 'target_offset' or value <= size, 'Target outside snapshot')
        require(key != 'expected' or value < 1 << (width * 8), 'Expected pointer overflow')
        fields.append(dict(field, offset=offset, **{key: value}))
    invariants = []
    require(isinstance(plan.get('invariants', []), list), 'Invariants must be a list')
    for inv in plan.get('invariants', []):
        mapping(inv, 'Invariant must be an object')
        offset, length = integer(inv.get('offset')), integer(inv.get('size'))
        require(offset + length <= size, 'Invariant outside snapshot')
        sha = inv.get('sha256')
        require(isinstance(sha, str) and len(sha) == 64 and
                all(c in '0123456789abcdef' for c in sha), 'Invalid invariant digest')
        invariants.append(dict(inv, offset=offset, size=length))
    require(isinstance(plan.get('finish_records', []), list), 'Finish inventory must be a list')
    for record in plan.get('finish_records', []):
        mapping(record, 'Finish record must be an object')
        require(integer(record.get('object_offset')) + width <= size and
                integer(record.get('class_name_offset')) < size, 'Finish record outside snapshot')
    return width, size, fields, invariants


def audit(plan, manifest, directory):
    width, size, fields, invariants = validate_plan(plan)
    mapping(manifest, 'Manifest must be an object')
    require(type(manifest.get('schema')) is int and manifest['schema'] == 1 and isinstance(manifest.get('snapshots'), list)
            and manifest['snapshots'], 'Invalid or empty snapshot manifest')
    require(isinstance(manifest.get('ordered', False), bool), 'ordered must be Boolean')
    snapshots, labels = [], set()
    for entry in manifest['snapshots']:
        mapping(entry, 'Snapshot entry must be an object')
        label = entry.get('label')
        require(isinstance(label, str) and label and label not in labels, 'Duplicate/empty snapshot label')
        labels.add(label)
        require(entry.get('phase') in ('raw', 'relocated'), 'Specify raw or relocated phase')
        base = integer(entry.get('base'))
        require(base + size <= 1 << (width * 8), 'Snapshot address overflow')
        require(isinstance(entry.get('path'), str) and entry['path'], 'Snapshot path must be a string')
        path = directory / entry['path']
        data = path.read_bytes()
        require(len(data) == size, f'{label}: snapshot size differs')
        for inv in invariants:
            require(digest(data[inv['offset']:inv['offset'] + inv['size']]) == inv['sha256'],
                    f'{label}: immutable input metadata differs')
        if entry['phase'] == 'raw' and plan.get('raw_sha256'):
            require(digest(data) == plan['raw_sha256'], f'{label}: raw identity differs')
        rows = []
        for field in fields:
            expected = base + field['target_offset'] if 'target_offset' in field else field['expected']
            require(expected < 1 << (width * 8), 'Expected pointer overflow')
            offset = field['offset']
            actual = int.from_bytes(data[offset:offset + width], plan['endian'])
            rows.append(dict(field, expected=expected, actual=actual,
                             status='match' if actual == expected else 'zero' if actual == 0 else 'different'))
        snapshots.append({'label': label, 'phase': entry['phase'], 'base': base,
                          'sha256': digest(data), 'identity': entry.get('identity', {}),
                          'counts': {status: sum(r['status'] == status for r in rows)
                                     for status in ('match', 'zero', 'different')}, 'fields': rows})
    relocated = [s for s in snapshots if s['phase'] == 'relocated']
    require(relocated, 'No relocated snapshot supplied')
    failures = [s for s in relocated if s['counts']['match'] != len(fields)]
    return {'schema': 1, 'plan_fields': len(fields), 'snapshots': snapshots,
            'earliest_compared_mismatch': failures[0]['label'] if manifest.get('ordered') and failures else None,
            'relocated_snapshots_match': not failures,
            'finish_records': plan.get('finish_records', []),
            'limits': 'Snapshot comparisons do not identify an executed store, writer, PC or first failing instruction.'}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    make = commands.add_parser('havok-v4-plan', help='Build a private plan from original raw bytes')
    make.add_argument('raw', type=Path)
    make.add_argument('--output', required=True, type=Path)
    compare = commands.add_parser('audit', help='Check every planned field in every snapshot')
    compare.add_argument('--plan', required=True, type=Path)
    compare.add_argument('--manifest', required=True, type=Path)
    compare.add_argument('--output', required=True, type=Path)
    args = parser.parse_args(argv)
    try:
        if args.command == 'havok-v4-plan':
            result = havok_v4_plan(args.raw.read_bytes())
            protect_inputs(args.output, [args.raw])
            exit_code = 0
        else:
            manifest = json.loads(args.manifest.read_text(encoding='utf-8'))
            directory = args.manifest.resolve().parent
            result = audit(json.loads(args.plan.read_text(encoding='utf-8')), manifest, directory)
            protect_inputs(args.output, [args.plan, args.manifest] +
                           [directory / entry['path'] for entry in manifest['snapshots']])
            exit_code = 0 if result['relocated_snapshots_match'] else 1
        args.output.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
        print(f'{args.command}: wrote {args.output}; fields={len(result.get("fields", [])) or result.get("plan_fields", 0)}')
        return exit_code
    except (AuditError, OSError, ValueError, KeyError, TypeError) as exc:
        print(f'pointer audit rejected input: {exc}', file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())
