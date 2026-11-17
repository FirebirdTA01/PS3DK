"""Receipt-only tests: never certify firmware behavior or target code execution."""
import importlib.util
from pathlib import Path
import unittest

SPEC = importlib.util.spec_from_file_location("spurs_verify", Path(__file__).parents[1] / "tools" / "verify.py")


def fixture():
    return {
        "schema": 1, "arm": "ON", "build": "a" * 64, "session": "0123456789abcdef",
        "submitted": 6, "completed": 6, "queue_complete": True, "complete": True,
        "queue_rc": 0, "close_rc": 0, "queue_shutdown_rc": 0, "join_rc": 0,
        "queue_exit": 0, "finalize_rc": 0, "shutdown_rc": 0,
        "jobs": [dict(job=i, lane=i+1, receipt_job=i, receipt_lane=i+1,
                      cookie="0123456789abcdef", receipt_cookie="0123456789abcdef",
                      ring=4096+4096*i, receipt_ring=4096+4096*i,
                      ctl=32768+256*i, receipt_ctl=32768+256*i,
                      present=True, post_fini=True, status=0, attach_rc=0, flush_rc=0, fini_rc=0,
                      checksum=[4232289247,1471454174,931044317,1698864092,1609800667,2655427546][i],
                      expected_checksum=[4232289247,1471454174,931044317,1698864092,1609800667,2655427546][i], iterations=65536,
                      tag=3, mask_before=8, mask_after=8, event_before=0, event_after=0,
                      guards=True, receipt_guards=True, desc_count=5,
                      writer_ls=4096, writer_bytes=512, staging_ls=4864, staging_bytes=320,
                      dec=[1000, 900, 800], lane_end_called=True, lane_end_rc=0,
                      final_written=1, fin_state=1, events=7, lost=0,
                      have_post_join=1, post_state=2, post_next_seq=7,
                      post_older_lost=0, post_refused=0) for i in range(6)]}


class ReceiptTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if SPEC.origin and Path(SPEC.origin).exists():
            cls.module = importlib.util.module_from_spec(SPEC)
            SPEC.loader.exec_module(cls.module)
        else:
            cls.module = None

    def verify(self, receipt):
        self.assertIsNotNone(self.module, "receipt verifier has not been implemented")
        return self.module.verify(receipt, "ON", "a" * 64, "0123456789abcdef")

    def test_valid_receipt_reports_observations_not_hardware_proof(self):
        result = self.verify(fixture())
        self.assertEqual(result["receipt"], "PASS")
        self.assertEqual(result["clock"], "OBSERVED_DECREASING_ONLY")
        self.assertEqual(result["target_execution"], "NOT_ESTABLISHED_BY_RECEIPT_VERIFIER")

    def test_failed_fini_never_authorizes_retirement(self):
        row = fixture(); row["jobs"][0]["fini_rc"] = -21
        with self.assertRaisesRegex(ValueError, "retirement"):
            self.verify(row)

    def test_missing_receipt_never_authorizes_retirement(self):
        row = fixture(); row["jobs"][0]["present"] = False
        with self.assertRaisesRegex(ValueError, "retirement"):
            self.verify(row)

    def test_completion_without_lane_identity_never_authorizes_retirement(self):
        for key, value in [("receipt_lane", 99), ("receipt_cookie", "0"*16),
                           ("receipt_ring", 128), ("receipt_ctl", 128), ("receipt_job", 99),
                           ("post_fini", False)]:
            with self.subTest(key=key):
                row=fixture(); row["jobs"][0][key]=value
                with self.assertRaisesRegex(ValueError, "retirement"):
                    self.verify(row)

    def test_queue_completion_is_required_even_with_fini_receipt(self):
        row=fixture(); row["queue_complete"]=False
        with self.assertRaisesRegex(ValueError, "completion"):
            self.verify(row)

    def test_failed_fini_without_release_is_reported_as_failed_run(self):
        row=fixture(); row["jobs"][0].update(fini_rc=-21, post_fini=False, lane_end_called=False)
        with self.assertRaises(ValueError):
            self.verify(row)

    def test_constant_and_reset_clocks_are_inconclusive(self):
        for dec, verdict in [([1000,1000,1000], "INCONCLUSIVE_CONSTANT_DEC"),
                             ([1000,1100,900], "INCONCLUSIVE_RESET_OR_WRAP"),
                             ([1000,1000,900], "INCONCLUSIVE_PARTIAL_STALL")]:
            with self.subTest(dec=dec):
                row=fixture(); row["jobs"][0]["dec"]=dec
                self.assertEqual(self.verify(row)["clock"], verdict)

    def test_duplicate_lanes_rejected(self):
        row=fixture(); row["jobs"][1]["lane"]=1; row["jobs"][1]["receipt_lane"]=1
        with self.assertRaisesRegex(ValueError,"unique"):
            self.verify(row)

    def test_guards_masks_counters_and_cleanup_are_required(self):
        cases=[("guards",False), ("receipt_guards",False), ("mask_after",0),
               ("event_after",1), ("lost",1), ("post_refused",1), ("checksum",124),
               ("lane_end_rc",-19), ("final_written",0), ("desc_count",0),
               ("staging_ls",4096), ("post_state",1)]
        for key,value in cases:
            with self.subTest(key=key):
                row=fixture(); row["jobs"][0][key]=value
                with self.assertRaises(ValueError): self.verify(row)
        for key in ("close_rc","queue_shutdown_rc","join_rc","finalize_rc","shutdown_rc"):
            with self.subTest(key=key):
                row=fixture(); row[key]=-1
                with self.assertRaises(ValueError): self.verify(row)

    def test_wrong_pin_bool_integer_and_incomplete_receipt_rejected(self):
        for key,value in [("build","b"*64),("session","0"*16),("complete",False),("submitted",True)]:
            row=fixture();row[key]=value
            with self.assertRaises(ValueError): self.verify(row)

    def test_aliasing_ring_or_control_is_not_a_distinct_producer(self):
        for key in ("ring", "ctl"):
            row=fixture(); row["jobs"][1][key]=row["jobs"][0][key]
            row["jobs"][1]["receipt_"+key]=row["jobs"][0][key]
            with self.assertRaisesRegex(ValueError,"unique"):
                self.verify(row)

    def test_off_control_has_no_writer_or_retirement(self):
        self.assertIsNotNone(self.module, "receipt verifier has not been implemented")
        row=fixture();row["arm"]="OFF"
        for j in row["jobs"]:
            j.update(lane=0,receipt_lane=0,ring=0,receipt_ring=0,ctl=0,receipt_ctl=0,
                     post_fini=False,attach_rc=-17,flush_rc=-17,fini_rc=-17,lane_end_called=False)
        self.assertEqual(self.module.verify(row,"OFF","a"*64,"0123456789abcdef")["receipt"],"PASS")

    def test_bool_is_not_a_receipt_job_identity(self):
        row=fixture();row["jobs"][0]["receipt_job"]=False
        with self.assertRaises(ValueError): self.verify(row)

    def test_duplicate_json_keys_rejected(self):
        self.assertIsNotNone(self.module, "receipt verifier has not been implemented")
        with self.assertRaisesRegex(ValueError,"duplicate"):
            self.module.unique([("schema",1),("schema",1)])

    def test_agreeing_wrong_checksum_is_not_evidence_of_work(self):
        row=fixture();row["jobs"][0].update(checksum=123,expected_checksum=123)
        with self.assertRaisesRegex(ValueError,"checksum"):
            self.verify(row)


if __name__ == "__main__":
    unittest.main()
