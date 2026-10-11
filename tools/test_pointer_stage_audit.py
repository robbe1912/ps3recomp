#!/usr/bin/env python3
"""Synthetic red/green and malformed-input checks; no game payloads."""
import copy
import json
import struct
import tempfile
import unittest
from pathlib import Path

import pointer_stage_audit as audit


def packfile():
    raw = bytearray(160)
    raw[:8] = bytes.fromhex('57e0e05710c0c010')
    struct.pack_into('>I', raw, 12, 4)
    raw[16:20] = bytes([4, 0, 1, 1])
    struct.pack_into('>I', raw, 20, 1)
    raw[64:72] = b'__data__'
    struct.pack_into('>7I', raw, 84, 112, 16, 24, 36, 48, 48, 48)
    struct.pack_into('>2I', raw, 128, 0, 4)
    struct.pack_into('>3I', raw, 136, 4, 0, 8)
    struct.pack_into('>3I', raw, 148, 8, 0, 12)
    return bytes(raw)


class PointerAuditTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.raw = packfile()
        self.plan = audit.havok_v4_plan(self.raw)
        self.manifest = {'schema': 1, 'ordered': True, 'snapshots': []}

    def snapshot(self, label, base, phase='relocated', missing=False):
        data = bytearray(self.raw)
        if phase == 'relocated':
            struct.pack_into('>I', data, 112, 0 if missing else base + 116)
            struct.pack_into('>I', data, 116, base + 120)
        (self.root / label).write_bytes(data)
        self.manifest['snapshots'].append({'label': label, 'path': label, 'base': base, 'phase': phase})

    def test_missing_pointer_is_red_then_rebased_correct_snapshot_is_green(self):
        self.snapshot('red', 0, missing=True)
        self.snapshot('green', 0x20000)
        result = audit.audit(self.plan, self.manifest, self.root)
        self.assertFalse(result['relocated_snapshots_match'])
        self.assertEqual(result['earliest_compared_mismatch'], 'red')
        self.assertEqual(result['snapshots'][0]['counts'], {'match': 1, 'zero': 1, 'different': 0})
        self.assertEqual(result['snapshots'][1]['counts']['match'], 2)
        self.assertEqual(len(result['snapshots'][0]['fields']), 2)
        self.assertEqual(len(result['finish_records']), 1)
        self.manifest['snapshots'].pop(0)
        self.assertTrue(audit.audit(self.plan, self.manifest, self.root)['relocated_snapshots_match'])

    def test_raw_is_pending_and_cannot_be_the_only_capture(self):
        self.snapshot('raw', 0, phase='raw')
        with self.assertRaises(audit.AuditError):
            audit.audit(self.plan, self.manifest, self.root)
        self.snapshot('post', 0x10000)
        self.assertIsNone(audit.audit(self.plan, self.manifest, self.root)['earliest_compared_mismatch'])

    def test_wrong_resource_metadata_and_wrong_raw_identity_are_rejected(self):
        self.snapshot('post', 0)
        data = bytearray((self.root / 'post').read_bytes())
        data[128] ^= 1
        (self.root / 'post').write_bytes(data)
        with self.assertRaisesRegex(audit.AuditError, 'metadata'):
            audit.audit(self.plan, self.manifest, self.root)
        self.manifest['snapshots'].clear()
        self.snapshot('raw', 0, phase='raw')
        data = bytearray(self.raw); data[120] = 1
        (self.root / 'raw').write_bytes(data)
        with self.assertRaisesRegex(audit.AuditError, 'raw identity'):
            audit.audit(self.plan, self.manifest, self.root)

    def test_unordered_snapshots_do_not_claim_chronology(self):
        self.snapshot('post', 0, missing=True)
        self.manifest['ordered'] = False
        self.assertIsNone(audit.audit(self.plan, self.manifest, self.root)['earliest_compared_mismatch'])

    def test_generic_little_endian_64bit_and_literal_pointer(self):
        plan = {'schema': 1, 'size': 16, 'pointer_bytes': 8, 'endian': 'little',
                'fields': [{'offset': 0, 'target_offset': 16}, {'offset': 8, 'expected': 123}]}
        (self.root / 'post').write_bytes(struct.pack('<QQ', 0x100000010, 123))
        manifest = {'schema': 1, 'snapshots': [{'label': 'post', 'path': 'post', 'base': 0x100000000, 'phase': 'relocated'}]}
        self.assertTrue(audit.audit(plan, manifest, self.root)['relocated_snapshots_match'])

    def test_plan_overlap_overflow_and_ambiguous_expected_rejected(self):
        for mutation in ('overlap', 'overflow', 'ambiguous'):
            plan = copy.deepcopy(self.plan)
            if mutation == 'overlap':plan['fields'][1]['offset'] = 113
            if mutation == 'overflow':plan['fields'][0]['expected'] = 1 << 32; del plan['fields'][0]['target_offset']
            if mutation == 'ambiguous':plan['fields'][0]['expected'] = 1
            self.snapshot(mutation, 0)
            with self.assertRaises(audit.AuditError):
                audit.audit(plan, self.manifest, self.root)

    def test_malformed_packfile_rejected(self):
        for offset, value in ((12, 5), (20, 1000), (84, 1), (128, 16), (140, 10)):
            raw = bytearray(self.raw); struct.pack_into('>I', raw, offset, value)
            with self.assertRaises(audit.AuditError):audit.havok_v4_plan(raw)
        with self.assertRaises(audit.AuditError):audit.havok_v4_plan(self.raw[:63])

    def test_cli_exit_status_and_private_report(self):
        self.snapshot('post', 0, missing=True)
        plan, manifest, output = (self.root / name for name in ('plan.json', 'manifest.json', 'report.json'))
        plan.write_text(json.dumps(self.plan)); manifest.write_text(json.dumps(self.manifest))
        args = ['audit', '--plan', str(plan), '--manifest', str(manifest), '--output', str(output)]
        self.assertEqual(audit.main(args), 1)
        self.assertEqual(json.loads(output.read_text())['plan_fields'], 2)
        self.snapshot('good', 0x10000); self.manifest['snapshots'].pop(0)
        manifest.write_text(json.dumps(self.manifest))
        self.assertEqual(audit.main(args), 0)
        self.manifest['snapshots'][0]['base'] = 0xffffffff
        manifest.write_text(json.dumps(self.manifest))
        self.assertEqual(audit.main(args), 2)

    def test_wrong_json_shapes_raise_audit_errors(self):
        self.snapshot('post', 0)
        for bad in ([], None, {'schema': True}, dict(self.plan, fields=[None]),
                    dict(self.plan, invariants=[None]), dict(self.plan, finish_records=[None])):
            with self.assertRaises(audit.AuditError):audit.audit(bad, self.manifest, self.root)
        for bad in ([], None, dict(self.manifest, schema=True), dict(self.manifest, snapshots=[None]),
                    dict(self.manifest, snapshots=[dict(self.manifest['snapshots'][0], path=None)])):
            with self.assertRaises(audit.AuditError):audit.audit(self.plan, bad, self.root)

    def test_cli_refuses_input_overwrite_including_hard_links(self):
        raw = self.root / 'raw'; raw.write_bytes(self.raw)
        args = ['havok-v4-plan', str(raw), '--output', str(raw)]
        self.assertEqual(audit.main(args), 2)
        self.assertEqual(raw.read_bytes(), self.raw)
        import os
        alias = self.root / 'alias'; os.link(raw, alias)
        args[-1] = str(alias)
        self.assertEqual(audit.main(args), 2)
        self.assertEqual(raw.read_bytes(), self.raw)


if __name__ == '__main__':
    unittest.main()
