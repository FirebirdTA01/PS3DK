"""Stage 2 sequential specification controls; not target ordering evidence."""
import importlib.util
import itertools
from pathlib import Path
import unittest

HERE = Path(__file__).resolve().parent


class TargetLifetime(unittest.TestCase):
    def model(self, **kwargs):
        path = HERE / 'auto_target_model.py'
        self.assertTrue(path.exists(), 'cumulative-label and automatic-init model is absent')
        spec = importlib.util.spec_from_file_location('auto_target_model', path)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        return mod.Capture(**kwargs)

    def ready(self, **kwargs):
        m = self.model(**kwargs)
        self.assertEqual(m.gcm_init(clean=True, managed=True), 0)
        self.assertEqual(m.state, 'ATTACHED')
        return m

    def test_init_attaches_without_public_attach_call(self):
        m = self.ready()
        self.assertEqual(m.configs, [('ATTACHED', 1)])
        self.assertEqual(m.override_calls, 0)
        self.assertEqual(m.submit(), 1)

    def test_unknown_lease_is_a_named_capture_outcome(self):
        m = self.model()
        self.assertEqual(m.gcm_init(clean=True, managed=False), 0)
        self.assertEqual(m.configs[-1][0], 'RESOURCE_LEASE_UNVERIFIED')
        self.assertEqual(m.configs, [('RESOURCE_LEASE_UNVERIFIED', 1)])
        self.assertEqual(m.submit(), 0)
        self.assertEqual(m.app_calls, 1)
        self.assertEqual(m.marker_words, 0)

    def test_init_failure_preserves_application_result(self):
        m = self.model()
        self.assertEqual(m.gcm_init(clean=True, managed=True, application_rc=-17), -17)
        self.assertEqual(m.configs, [('GCM_INIT_FAILED', 1)])
        self.assertEqual(m.marker_words, 0)

    def test_successful_app_init_preserved_when_observer_refuses(self):
        m = self.model()
        self.assertEqual(m.gcm_init(clean=True, managed=False), 0, 'APP_INIT_RETURN')
        self.assertEqual(m.configs[-1][0], 'RESOURCE_LEASE_UNVERIFIED')

    def test_missing_independent_drain_refuses_before_claim(self):
        m = self.model(independent_drain=False)
        self.assertEqual(m.gcm_init(clean=True, managed=True), 0)
        self.assertEqual(m.configs[-1][0], 'NO_INDEPENDENT_DRAIN')
        self.assertFalse(m.leased)

    def test_dirty_context_does_not_reset_old_label(self):
        m = self.model()
        m.label = 2
        self.assertEqual(m.gcm_init(clean=False, managed=True), 0)
        self.assertEqual(m.configs[-1][0], 'PRIOR_WRITES_UNKNOWN')
        self.assertEqual(m.label, 2)
        self.assertEqual(m.submit(), 0)

    def test_optional_override_before_first_marker(self):
        m = self.ready()
        self.assertEqual(m.override(first=512, count=32, op_label=236, clock_label=237), 'ATTACHED')
        self.assertEqual((m.first, m.count, m.op_label, m.clock_label), (512, 32, 236, 237))
        self.assertEqual(m.override_calls, 1)
        self.assertEqual(m.submit(), 1)

    def test_override_after_issuance_preserves_live_lease(self):
        m = self.ready()
        m.submit()
        self.assertEqual(m.override(first=512, count=32, op_label=236, clock_label=237), 'LIVE_LEASE')
        self.assertEqual((m.first, m.op_label), (1024, 247))

    def test_accepted_configuration_is_immutable(self):
        m = self.ready()
        m.config_accepted()
        self.assertEqual(m.override(first=512, count=32, op_label=236, clock_label=237), 'LIVE_LEASE')
        self.assertEqual((m.first, m.op_label), (1024, 247))

    def test_default_resource_collision_is_visible(self):
        for foreign in [(1024, 2, 240), (0, 2, 246), (0, 2, 247)]:
            m = self.model(foreign=[foreign])
            self.assertEqual(m.gcm_init(clean=True, managed=True), 0)
            self.assertEqual(m.configs[-1][0], 'RESOURCE_CONFLICT')
            self.assertFalse(m.leased)

    def test_invalid_override_cannot_destroy_default_lease(self):
        for args in [(2040, 32, 236, 237), (512, 17, 236, 237), (512, 32, 65, 237), (512, 32, 236, 236)]:
            m = self.ready()
            self.assertEqual(m.override(first=args[0], count=args[1], op_label=args[2], clock_label=args[3]), 'BAD_OPTIONS')
            self.assertEqual((m.first, m.count), (1024, 1024))

    def test_cumulative_label_recovers_missed_intermediate_values(self):
        m = self.ready()
        self.assertEqual([m.submit(), m.submit()], [1, 2])
        self.assertTrue(m.prepare_publication(4))
        self.assertTrue(m.put(4))
        self.assertTrue(m.device(4, {1: (100, 101), 2: (102, 103)}))
        self.assertEqual(m.observe(4, 4), 'COMPLETE')
        self.assertEqual(m.complete_ids(), [1, 2])
        self.assertEqual(m.pending, 2)
        self.assertTrue(m.sink(1, whole=True))
        self.assertTrue(m.sink(2, whole=True))
        self.assertEqual((m.pending, m.held), (0, 0))

    def test_put_requires_metadata_prepublication(self):
        m = self.ready()
        m.submit()
        self.assertFalse(m.put(2))
        self.assertEqual(m.put_frontier, 0)
        self.assertFalse(m.device(2, {1: (100, 101)}))

    def test_prepublication_alone_is_not_device_completion(self):
        m = self.ready()
        m.submit()
        m.prepare_publication(2)
        self.assertEqual(m.observe(2, 2), 'DEFER')
        self.assertEqual(m.complete_ids(), [])

    def test_stale_publication_snapshot_defers_without_quarantine(self):
        m = self.ready()
        m.submit()
        m.prepare_publication(2)
        m.put(2)
        m.device(2, {1: (10, 20)})
        self.assertEqual(m.observe(2, 0), 'DEFER')
        self.assertEqual(m.state, 'ATTACHED')
        self.assertEqual(m.observe(2, 2), 'COMPLETE')

    def test_future_serial_and_regression_do_not_release(self):
        m = self.ready()
        m.submit()
        self.assertEqual(m.observe(4, 4), 'INVALID_LABEL')
        self.assertEqual(m.held, 1)
        self.assertFalse(m.sink(1, whole=True))

    def test_label_regression_after_latched_completion(self):
        m = self.ready()
        m.submit()
        m.prepare_publication(2)
        m.put(2)
        m.device(2, {1: (10, 20)})
        self.assertEqual(m.observe(2, 2), 'COMPLETE')
        self.assertEqual(m.observe(1, 2), 'INVALID_LABEL')
        self.assertEqual(m.latched, 2)

    def test_failed_sink_ack_and_stable_read_required(self):
        m = self.ready(pairs=1)
        m.submit()
        m.prepare_publication(2)
        m.put(2)
        m.device(2, {1: (10, 20)})
        self.assertEqual(m.observe(2, 2, stable=False), 'READ_REFUSED')
        self.assertFalse(m.sink(1, whole=True))
        self.assertEqual(m.observe(2, 2), 'COMPLETE')
        self.assertFalse(m.sink(1, whole=False))
        self.assertEqual(m.submit(), 0)
        self.assertEqual(m.held, 1)
        self.assertTrue(m.sink(1, whole=True))
        self.assertEqual(m.submit(), 2)

    def test_bad_report_order_is_quarantined(self):
        m = self.ready()
        m.submit()
        m.prepare_publication(2)
        m.put(2)
        m.device(2, {1: (20, 10)})
        self.assertEqual(m.observe(2, 2), 'CLOCK_REGRESSION')
        self.assertFalse(m.sink(1, whole=True))
        self.assertEqual(m.held, 1)

    def test_completed_report_fact_cannot_be_overwritten(self):
        m = self.ready()
        m.submit()
        m.prepare_publication(2)
        m.put(2)
        m.device(2, {1: (10, 20)})
        m.observe(2, 2)
        self.assertFalse(m.device(2, {1: (30, 20)}), 'IMMUTABLE_REPORT')
        self.assertTrue(m.sink(1, whole=True))
        self.assertEqual(m.tickets[1]['raw'], (10, 20))

    def test_live_collision_and_reset_quarantine_outstanding(self):
        for reason in ['RESOURCE_CONFLICT', 'CONTEXT_RESET']:
            m = self.ready()
            m.submit()
            m.prepare_publication(2)
            m.put(2)
            m.device(2, {1: (10, 20)})
            self.assertTrue(callable(getattr(m, 'invalidate', None)), 'LIVE_INVALIDATION_MISSING')
            m.invalidate(reason)
            self.assertEqual(m.configs[-1][0], reason)
            self.assertEqual(m.observe(2, 2), 'STOPPED', 'LIVE_INVALIDATION')
            self.assertFalse(m.sink(1, whole=True))
            self.assertEqual(m.submit(), 0)
            self.assertEqual((m.app_calls, m.held), (2, 1))

    def test_clock_without_publication_cannot_retire(self):
        m = self.ready()
        m.clock_issue()
        self.assertFalse(m.clock_complete(1, stable=True, whole=True), 'CLOCK_PUBLICATION')
        self.assertFalse(m.teardown_safe())

    def test_clock_publication_snapshot_and_generation(self):
        m = self.ready(generation=2)
        m.clock_issue()
        self.assertFalse(m.clock_put(1), 'CLOCK_PREPUBLICATION')
        m.clock_prepare(1)
        m.clock_put(1)
        self.assertFalse(m.clock_device(1, {1: 100}, generation=1), 'CLOCK_GENERATION')
        self.assertTrue(m.clock_device(1, {1: 200}, generation=2))
        self.assertFalse(m.clock_complete(1, stable=True, whole=True, publication_snapshot=0), 'CLOCK_SNAPSHOT')
        self.assertFalse(m.clock_complete(1, stable=False, whole=True, publication_snapshot=1))
        self.assertTrue(m.clock_complete(1, stable=True, whole=True, publication_snapshot=1))
        self.assertFalse(m.clock_device(1, {1: 300}, generation=2), 'CLOCK_IMMUTABLE')
        self.assertEqual(m.clocks[0]['raw'], 200)

    def test_clock_label_without_report_and_invalidation_cannot_retire(self):
        m = self.ready()
        m.clock_issue()
        m.clock_prepare(1)
        m.clock_put(1)
        self.assertTrue(m.clock_device(1, {}))
        self.assertFalse(m.clock_complete(1, stable=True, whole=True, publication_snapshot=1), 'CLOCK_REPORT')
        self.assertTrue(m.clock_device(1, {1: 100}))
        m.invalidate('CONTEXT_RESET')
        self.assertFalse(m.clock_complete(1, stable=True, whole=True, publication_snapshot=1), 'CLOCK_INVALIDATION')
        self.assertFalse(m.teardown_safe())

    def test_serial_exhaustion_preserves_app_work(self):
        m = self.ready()
        m.next_serial = 0xffffffff
        self.assertEqual(m.submit(), 0)
        self.assertEqual((m.app_calls, m.marker_words), (1, 0))
        self.assertEqual(m.exclusions, {'serial': 1})

    def test_seal_wins_attach_claim_and_failed_unwind(self):
        m = self.model()
        self.assertEqual(m.begin_init(clean=True, managed=True), 'ATTACHING')
        m.seal()
        self.assertEqual(m.finish_init(), 'STOPPED')
        self.assertEqual(m.state, 'SEALED')
        self.assertFalse(m.leased)
        self.assertEqual(m.submit(), 0)

    def test_shutdown_quarantines_but_keeps_unreported_disposition(self):
        m = self.ready()
        m.submit()
        m.seal()
        self.assertEqual((m.pending, m.held), (1, 1))
        self.assertTrue(m.sink_incomplete(1, whole=True))
        self.assertEqual((m.pending, m.held, m.written), (0, 1, 1))
        self.assertFalse(m.teardown_safe())

    def test_same_capture_cannot_reattach_or_zero_old_instance(self):
        m = self.ready()
        m.label = 2
        m.seal()
        self.assertEqual(m.gcm_init(clean=True, managed=True), 0)
        self.assertEqual(m.configs[-1][0], 'STOPPED')
        self.assertEqual(m.label, 2)

    def test_clock_slots_are_distinct_and_no_early_teardown(self):
        m = self.ready()
        self.assertEqual([m.clock_issue() for _ in range(16)], list(range(1024, 1040)))
        self.assertIsNone(m.clock_issue())
        self.assertFalse(m.teardown_safe())
        self.assertFalse(m.clock_complete(17, stable=True, whole=True))
        self.assertTrue(m.clock_prepare(16))
        self.assertTrue(m.clock_put(16))
        self.assertTrue(m.clock_device(16, {i: i * 100 for i in range(1, 17)}))
        for serial in range(1, 17):
            self.assertTrue(m.clock_complete(serial, stable=True, whole=True, publication_snapshot=16))
        self.assertTrue(m.teardown_safe())

    def test_incomplete_clock_metadata_does_not_free_lease(self):
        m = self.ready()
        m.clock_issue()
        m.clock_prepare(1)
        m.clock_put(1)
        m.clock_device(1, {1: 100})
        self.assertFalse(m.clock_complete(1, stable=True, whole=False, publication_snapshot=1))
        self.assertFalse(m.teardown_safe())

    def test_full_frame_queue_names_loss_and_never_overwrites(self):
        m = self.ready(frame_capacity=1)
        self.assertTrue(m.frame_close(0, visited=3, observed=2, excluded=1))
        self.assertFalse(m.frame_close(1, visited=9, observed=9, excluded=0))
        self.assertEqual(m.frames_lost, 1)
        self.assertEqual(m.frame_offer(), (0, 3, 2, 1))
        self.assertFalse(m.frame_ack(whole=False))
        self.assertEqual(m.frame_offer(), (0, 3, 2, 1))
        self.assertTrue(m.frame_ack(whole=True))
        self.assertIsNone(m.frame_offer())
        self.assertTrue(m.frame_close(2, visited=0, observed=0, excluded=0))
        self.assertEqual(m.frame_offer(), (2, 0, 0, 0))

    def test_bad_frame_conservation_cannot_publish(self):
        m = self.ready()
        self.assertFalse(m.frame_close(0, visited=3, observed=2, excluded=0))
        self.assertIsNone(m.frame_offer())

    def test_frame_identity_cannot_be_reused_after_ack_or_loss(self):
        m = self.ready(frame_capacity=1)
        m.frame_close(0, visited=0, observed=0, excluded=0)
        m.frame_close(1, visited=1, observed=0, excluded=1)
        m.frame_ack(whole=True)
        self.assertFalse(m.frame_close(1, visited=1, observed=1, excluded=0))
        self.assertFalse(m.frame_close(0, visited=0, observed=0, excluded=0))

    def test_finite_handoff_schedules_do_not_reuse_without_both_facts(self):
        for schedule in itertools.permutations('MDOS'):
            m = self.ready(pairs=1)
            m.submit()
            had_completion = had_ack = False
            for action in schedule:
                if action == 'M':
                    m.prepare_publication(2)
                    m.put(2)
                elif action == 'D':
                    m.device(2, {1: (10, 20)})
                elif action == 'O':
                    had_completion = m.observe(2, m.prepared) == 'COMPLETE'
                else:
                    had_ack = m.sink(1, whole=True)
                self.assertEqual(m.held == 0, had_completion and had_ack, schedule)


if __name__ == '__main__':
    unittest.main()
