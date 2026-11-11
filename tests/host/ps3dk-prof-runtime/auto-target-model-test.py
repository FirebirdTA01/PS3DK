"""Run the sequential target-admission model and semantic weakening controls."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

HERE = Path(__file__).resolve().parent
MUTANTS = {
    'manual-init-only': ('            self.finish_init()',
                         '            pass', 'test_init_attaches_without_public_attach_call'),
    'allow-unknown-lease': ('if not managed:', 'if False:', 'test_unknown_lease_is_a_named_capture_outcome'),
    'ignore-old-writes': ('if not clean:', 'if False:', 'test_dirty_context_does_not_reset_old_label'),
    'overwrite-sealed': ("if self.state != 'ATTACHING':", 'if False:', 'test_seal_wins_attach_claim_and_failed_unwind'),
    'ignore-prepublication': ('serial <= self.prepared:', 'serial < self.next_serial:', 'test_put_requires_metadata_prepublication'),
    'discard-publication-snapshot': ('serial > publication_snapshot or serial != self.label', 'serial != self.label', 'test_stale_publication_snapshot_defers_without_quarantine'),
    'ignore-whole-ack': ("if not whole or not t['complete'] or", "if not t['complete'] or", 'test_failed_sink_ack_and_stable_read_required'),
    'retire-incomplete': ("t['ack'] = True\n        self.written += 1\n        return True  # metadata",
                           "t['ack'] = t['retired'] = True\n        self.written += 1\n        return True  # metadata",
                           'test_shutdown_quarantines_but_keeps_unreported_disposition'),
    'ignore-clock-retirement': ("and all(t['ack'] for t in self.clocks)", '', 'test_incomplete_clock_metadata_does_not_free_lease'),
    'allow-serial-wrap': ('if self.next_serial > MAX_SERIAL - 2:', 'if False:', 'test_serial_exhaustion_preserves_app_work'),
    'allow-label-regression': (' or serial < self.latched:', ':', 'test_label_regression_after_latched_completion'),
    'override-published-config': (' or self.config_frozen:', ':', 'test_accepted_configuration_is_immutable'),
    'overwrite-frame-queue': ('if len(self.frames) >= self.frame_capacity:', 'if False:', 'test_full_frame_queue_names_loss_and_never_overwrites'),
    'hide-frame-conservation': (' or visited != observed + excluded:', ':', 'test_bad_frame_conservation_cannot_publish'),
    'reuse-lost-frame': ('ident <= self.last_frame or ', '', 'test_frame_identity_cannot_be_reused_after_ack_or_loss'),
    'replace-app-result': ('        return application_rc\n\n    def override', '        return result\n\n    def override', 'test_successful_app_init_preserved_when_observer_refuses'),
    'rewrite-report': (" or t['raw'] is not None:", ':', 'test_completed_report_fact_cannot_be_overwritten'),
    'ignore-invalidation': ('        self.seal()\n        self.reason(reason)', '        self.reason(reason)', 'test_live_collision_and_reset_quarantine_outstanding'),
    'clock-ignore-prepublication': ('serial <= self.clock_prepared:', 'serial <= len(self.clocks):', 'test_clock_publication_snapshot_and_generation'),
    'clock-ignore-generation': (' or generation != self.generation', '', 'test_clock_publication_snapshot_and_generation'),
    'clock-ignore-snapshot': ('serial > publication_snapshot or serial > self.clock_put_frontier', 'serial > self.clock_put_frontier', 'test_clock_publication_snapshot_and_generation'),
    'clock-ignore-report': ("if t['raw'] is None or t['ack']:", "if t['ack']:", 'test_clock_label_without_report_and_invalidation_cannot_retire'),
}

ASSERTIONS = {
    'replace-app-result': 'APP_INIT_RETURN', 'rewrite-report': 'IMMUTABLE_REPORT',
    'ignore-invalidation': 'LIVE_INVALIDATION', 'clock-ignore-prepublication': 'CLOCK_PREPUBLICATION',
    'clock-ignore-generation': 'CLOCK_GENERATION', 'clock-ignore-snapshot': 'CLOCK_SNAPSHOT',
    'clock-ignore-report': 'CLOCK_REPORT',
}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--mutants', action='store_true')
    args = ap.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    source = (HERE / 'auto_target_model.py').read_text(encoding='utf-8')
    controls = (HERE / 'auto_target_lifetime_test.py').read_bytes()
    receipts = []
    jobs = [('green', source, None)]
    if args.mutants:
        for name, (old, new, test) in MUTANTS.items():
            assert source.count(old) == 1, (name, source.count(old))
            jobs.append((name, source.replace(old, new), test))
    for name, code, test in jobs:
        folder = out / name
        folder.mkdir()
        (folder / 'auto_target_model.py').write_bytes(code.encode('utf-8'))
        (folder / 'auto_target_lifetime_test.py').write_bytes(controls)
        command = [sys.executable, '-B', str(folder / 'auto_target_lifetime_test.py')]
        if test:
            command.append('TargetLifetime.' + test)
        result = subprocess.run(command, capture_output=True, timeout=60)
        (folder / 'stdout.txt').write_bytes(result.stdout)
        (folder / 'stderr.txt').write_bytes(result.stderr)
        text = result.stderr.decode('utf-8', errors='replace')
        killed = bool(test and result.returncode == 1 and 'FAIL: ' + test in text and 'AssertionError' in text and 'ERROR:' not in text)
        if name in ASSERTIONS:
            killed = killed and ASSERTIONS[name] in text
        receipts.append(dict(name=name, argv=command, rc=result.returncode, intended_control=test, killed=killed,
                             source_sha256=hashlib.sha256(code.encode('utf-8')).hexdigest()))
        (out / 'commands.json').write_bytes((json.dumps(receipts, indent=2) + '\n').encode())
        assert killed if test else result.returncode == 0, (name, text)
        print(name, 'KILLED' if test else 'PASS', flush=True)
    print('Sequential host specification only; no device or memory-order proof.')


if __name__ == '__main__':
    main()
