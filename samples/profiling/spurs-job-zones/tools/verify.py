"""Check the finite sample's receipt, not firmware semantics or .prof contents.

Run the pinned public reader separately on the paired capture. Pin the receipt,
capture, SELF, ELF, archive and reader hashes in the run packet. A receipt alone
does not establish that target execution occurred, nor prove no hidden DEC reset.
"""
import argparse
import json
from pathlib import Path
import re


def need(ok, reason):
    if not ok:
        raise ValueError(reason)


def uint(value, maximum=(1 << 64)-1):
    return type(value) is int and 0 <= value <= maximum


def zero(value):
    return type(value) is int and value == 0


def unique(pairs):
    result = {}
    for key, value in pairs:
        need(key not in result, "duplicate JSON key")
        result[key] = value
    return result


def verify(r, arm, build, session):
    need(arm in ("ON", "OFF"), "arm")
    need(re.fullmatch(r"[0-9a-f]{64}", build) is not None, "build pin")
    need(re.fullmatch(r"[0-9a-f]{16}", session) is not None, "session pin")
    need(type(r) is dict and type(r.get("schema")) is int and r["schema"] == 1, "schema")
    need((r.get("arm"),r.get("build"),r.get("session")) == (arm,build,session), "receipt identity")
    need(r.get("complete") is True, "incomplete receipt")
    need(r.get("queue_complete") is True and zero(r.get("queue_rc")), "queue completion")
    need(all(uint(r.get(k)) and r[k] == 6 for k in ("submitted", "completed")), "job counts")
    for key in ("close_rc", "queue_shutdown_rc", "join_rc", "queue_exit", "finalize_rc", "shutdown_rc"):
        need(zero(r.get(key)), "cleanup: " + key)
    jobs = r.get("jobs")
    need(type(jobs) is list and len(jobs) == 6, "six jobs")
    lanes = set(); rings = set(); controls = set()
    clocks = []
    for index, j in enumerate(jobs):
        need(type(j) is dict and uint(j.get("job")) and j["job"] == index, "job identity")
        lane=j.get("lane")
        if arm == "ON":
            need(uint(lane,1024) and lane > 0 and lane not in lanes, "unique lane")
            lanes.add(lane)
            need(uint(j.get("ring")) and uint(j.get("ctl")) and
                 j["ring"] not in rings and j["ctl"] not in controls, "unique ring/control")
            rings.add(j["ring"]); controls.add(j["ctl"])
        else:
            need(zero(lane), "OFF lane")
        identity = (j.get("present") is True and uint(j.get("receipt_job")) and j.get("receipt_job") == index and
                    uint(j.get("receipt_lane")) and j.get("receipt_lane") == lane and j.get("cookie") == session and
                    j.get("receipt_cookie") == session and
                    uint(j.get("receipt_ring")) and uint(j.get("receipt_ctl")) and
                    j.get("receipt_ring") == j.get("ring") and j.get("receipt_ctl") == j.get("ctl"))
        authorized = identity and j.get("post_fini") is True and zero(j.get("fini_rc"))
        if j.get("lane_end_called") is True:
            need(arm == "ON" and authorized, "unauthorized retirement")
        need(identity, "receipt identity/retirement")
        need(j.get("guards") is True and j.get("receipt_guards") is True, "canary")
        need(zero(j.get("status")), "job status")
        expected=(4232289247,1471454174,931044317,1698864092,1609800667,2655427546)[index]
        need(uint(j.get("checksum"),0xffffffff) and uint(j.get("expected_checksum"),0xffffffff) and
             j["checksum"] == j["expected_checksum"] == expected, "checksum")
        need(uint(j.get("iterations")) and j["iterations"] == 65536, "work iterations")
        need(uint(j.get("tag"),31), "tag")
        for before, after in (("mask_before","mask_after"),("event_before","event_after")):
            need(uint(j.get(before),0xffffffff) and j.get(before) == j.get(after), "mask preservation")
        if arm == "ON":
            need(authorized and j.get("lane_end_called") is True, "retirement missing")
            for key in ("attach_rc", "flush_rc", "fini_rc", "lane_end_rc", "lost", "post_older_lost", "post_refused"):
                need(zero(j.get(key)), "producer/final counter: " + key)
            for key, value in dict(final_written=1,fin_state=1,events=7,have_post_join=1,post_state=2,post_next_seq=7,desc_count=5).items():
                need(uint(j.get(key)) and j[key] == value, "producer/final counter: " + key)
            for key in ("ring", "ctl"):
                need(uint(j.get(key)) and j[key] > 0 and j[key] % 128 == 0, "EA alignment")
            wa, wb, sa, sb = [j.get(k) for k in ("writer_ls","writer_bytes","staging_ls","staging_bytes")]
            need(all(uint(v,0x40000) for v in (wa,wb,sa,sb)) and wa % 128 == 0 and
                 sa % 16 == 0 and wb > 0 and sb == 320 and wa+wb <= 0x40000 and
                 sa+sb <= 0x40000 and (wa+wb <= sa or sa+sb <= wa), "LS allocation")
        else:
            need(j.get("lane_end_called") is False and j.get("post_fini") is False and
                 all(type(j.get(k)) is int and j[k] == -17 for k in ("attach_rc","flush_rc","fini_rc")), "OFF producer")
        dec = j.get("dec")
        need(type(dec) is list and len(dec) == 3 and all(uint(v,0xffffffff) for v in dec), "DEC samples")
        if dec[0] == dec[1] == dec[2]:
            clocks.append("INCONCLUSIVE_CONSTANT_DEC")
        elif dec[0] < dec[1] or dec[1] < dec[2]:
            clocks.append("INCONCLUSIVE_RESET_OR_WRAP")
        elif dec[0] == dec[1] or dec[1] == dec[2]:
            clocks.append("INCONCLUSIVE_PARTIAL_STALL")
        else:
            clocks.append("OBSERVED_DECREASING_ONLY")
    clock=next((c for c in clocks if c != "OBSERVED_DECREASING_ONLY"),"OBSERVED_DECREASING_ONLY")
    return dict(receipt="PASS", clock=clock, job_clocks=clocks,
                target_execution="NOT_ESTABLISHED_BY_RECEIPT_VERIFIER",
                capture_semantics="NOT_CHECKED_RUN_PINNED_READER", actual_overlap="NOT_ESTABLISHED",
                timing_scope="raw decrementer observations only; no wall-time or no-hidden-reset claim")


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("receipt",type=Path)
    parser.add_argument("--arm",choices=("ON","OFF"),required=True)
    parser.add_argument("--build",required=True)
    parser.add_argument("--session",required=True)
    args=parser.parse_args()
    try:
        result=verify(json.loads(args.receipt.read_text(encoding="utf-8"),object_pairs_hook=unique),
                      args.arm,args.build,args.session)
    except (ValueError, OSError) as exc:
        print(json.dumps(dict(receipt="FAIL",reason=str(exc))))
        return 1
    print(json.dumps(result,indent=2))
    return 2 if result["clock"].startswith("INCONCLUSIVE") else 0


if __name__ == "__main__":
    raise SystemExit(main())
