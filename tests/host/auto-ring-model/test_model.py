#!/usr/bin/env python3
"""Private report-ring protocol, independent ledger and adversarial schedules."""
import argparse
import copy
from dataclasses import dataclass, field
import json
from pathlib import Path
import struct
import subprocess
import sys


def check(value, name):
    if not value:
        raise AssertionError(name)


@dataclass
class Config:
    blocks: int = 2
    pairs: int = 2
    ring_bytes: int = 256
    metadata: int = 8
    credits: int = 4
    limit: int = 1000000

    @property
    def slots(self):
        return self.ring_bytes // (32 * self.pairs)

    def command(self):
        return "reset " + " ".join(str(x) for x in vars(self).values())


@dataclass
class Claim:
    seq: int
    frame: int
    window: int
    ids: list = field(default_factory=list)
    state: str = "OPEN"
    credit: bool = True
    local: bool = False


class Ledger:
    """Logical transactions, independent of C arrays and the byte encoder.

    Commands update this ledger BEFORE the implementation's response is read.
    No ownership or expected metadata is learned from model snapshots.
    """
    def __init__(self, cfg):
        self.cfg = cfg
        self.claims = {}
        self.retired = set()
        self.active = None
        self.issued = self.executed = self.visits = self.reserved = 0
        self.next = 1
        self.frame, self.window, self.last_window = 0, 1, 1
        self.stopped = self.invalid = False
        self.exclusions = {}
        self.last_id = 0

    def live(self):
        return [c for n, c in self.claims.items() if n not in self.retired]

    def seal(self):
        if self.active is not None:
            self.claims[self.active].state = "SEALED"
            self.active = None

    def step(self, command):
        parts = command.split()
        op, a = parts[0], [int(v) for v in parts[1:]]
        cfg, live = self.cfg, self.live()
        if op in ("draw", "uncertain"):
            check(a[0] > self.last_id, "driver_unique_visit_ids")
            self.last_id = a[0]
            self.visits += 1
            reason = None
            if self.stopped:
                reason = "STOPPED"
            elif self.invalid:
                reason = "REPLAY"
            elif not self.window:
                reason = "WINDOW_CLOSED"
            elif self.active is None and any((c.seq-1) % cfg.slots == self.issued % cfg.slots for c in live):
                reason = "RING_FULL"
            elif sum(len(c.ids) for c in live) == cfg.metadata:
                reason = "METADATA_FULL"
            elif self.active is None and sum(c.credit for c in live) == cfg.credits:
                reason = "COMMAND_FULL"
            elif self.active is None and self.issued == cfg.limit:
                reason = "SERIAL_FULL"
            if reason:
                self.exclusions[reason] = self.exclusions.get(reason, 0) + 1
                return reason
            if self.active is None:
                self.issued += 1
                self.active = self.issued
                self.claims[self.active] = Claim(self.issued, self.frame, self.window)
            c = self.claims[self.active]
            c.ids.append(a[0])
            self.reserved += 1
            if op == "uncertain":
                c.state = "QUARANTINE"
                self.active = None
                return "QUARANTINE"
            if len(c.ids) == cfg.pairs:
                self.seal()
            return "OK"
        if op == "flush":
            self.seal()
            return "OK"
        if op == "frame":
            if a[0] <= self.frame or self.stopped:
                return "BOUNDARY"
            self.seal()
            self.frame = a[0]
            return "OK"
        if op == "window":
            if self.stopped or a[0] == self.window or (a[0] and a[0] <= self.last_window):
                return "BOUNDARY"
            self.seal()
            self.window = a[0]
            self.last_window = max(self.last_window, a[0])
            return "OK"
        if op == "stop":
            self.seal()
            self.stopped = True
            return "OK"
        if op == "finish":
            return "OK" if self.stopped and not live and not self.invalid else "INCOMPLETE"
        if op == "replay":
            self.invalid = True
            if a[0] in self.claims and a[0] not in self.retired:
                self.claims[a[0]].state = "QUARANTINE"
                if self.active == a[0]:
                    self.active = None
            return "REPLAY"
        c = self.claims.get(a[0])
        if c is None or a[0] in self.retired:
            return "GENERATION"
        if op == "fault":
            c.state = "QUARANTINE"
            if self.active == c.seq:
                self.active = None
            return "QUARANTINE"
        if op == "execute":
            if c.state != "SEALED" or c.seq != self.executed + 1:
                return "ORDER"
            if any(old.local and (old.seq-1) % cfg.blocks == (c.seq-1) % cfg.blocks for old in live):
                return "LOCAL_BUSY"
            c.local, c.state = True, "EXECUTING"
            self.executed += 1
            return "OK"
        transitions = {"reports": ("EXECUTING", "REPORTS"), "copy": ("REPORTS", "COPIED")}
        if op in transitions:
            before, after = transitions[op]
            if c.state != before:
                return "ORDER"
            c.state = after
            if op == "copy":
                c.local = False
            return "OK"
        if op == "publish":
            if a[1] != c.seq:
                return "LABEL"
            if c.state != "COPIED":
                return "ORDER"
            c.credit, c.state = False, "READY"
            return "OK"
        if op == "read":
            if c.seq != self.next or c.state not in ("READY", "READING"):
                return "ORDER"
            c.state = "READING"
            return "OK"
        if op == "ack":
            if c.state != "READING" or c.seq != self.next:
                return "ORDER"
            if a[1] != 48 + len(c.ids) * 56:
                return "SHORT"
            self.retired.add(c.seq)
            self.next += 1
            return "OK"
        raise AssertionError("unknown driver command " + command)

    def expected(self):
        live = self.live()
        locals_ = [0] * self.cfg.blocks
        for c in live:
            if c.local:
                locals_[(c.seq-1) % self.cfg.blocks] = c.seq
        return dict(issued=self.issued, executed=self.executed, next=self.next,
                    visits=self.visits, reserved=self.reserved, exclusions=self.exclusions,
                    frame=self.frame, window=self.window, stopped=int(self.stopped),
                    invalid=int(self.invalid), locals=locals_,
                    metadata=sum(len(c.ids) for c in live), commands=sum(c.credit for c in live),
                    ring_bytes=self.cfg.ring_bytes, ring_slots=self.cfg.slots,
                    slots=[dict(seq=c.seq, state=c.state, ids=c.ids, frame=c.frame,
                                window=c.window, credit=int(c.credit), local=int(c.local))
                           for c in sorted(live, key=lambda c: c.seq)])


class Runner:
    def __init__(self, binary, out):
        self.log = (out / "trace.jsonl").open("w")
        self.errors = (out / "stderr.txt").open("w")
        self.p = subprocess.Popen([str(binary)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=self.errors, text=True, bufsize=1)
        self.steps = self.resets = 0

    def raw(self, command):
        self.p.stdin.write(command + "\n")
        self.p.stdin.flush()
        line = self.p.stdout.readline()
        if not line:
            raise RuntimeError("model exited; crash does not count as a mutant kill")
        row = json.loads(line)
        self.log.write(json.dumps(dict(command=command, response=row)) + "\n")
        self.steps += 1
        return row

    def reset(self, cfg=None):
        self.ledger = Ledger(cfg or Config())
        self.blobs = {}
        row = self.raw(self.ledger.cfg.command())
        check(row.pop("rc") == "OK", "attach_allocation")
        check(row == self.ledger.expected(), "empty_ledger")
        self.resets += 1

    def do(self, command, want=None, name="ledger_transition"):
        rc = self.ledger.step(command)
        if want:
            check(rc == want, "test_spec_" + name)
        row = self.raw(command)
        check(row.pop("rc") == rc, name)
        wire = row.pop("wire", None)
        expected = self.ledger.expected()
        for key in expected:
            check(row.get(key) == expected[key], name + ":" + key)
        check(set(row) == set(expected), "snapshot_schema")
        l = self.ledger
        check(l.visits == l.reserved + sum(l.exclusions.values()), "visit_conservation")
        check(l.reserved == sum(len(c.ids) for c in l.claims.values()), "claim_conservation")
        check(l.reserved == expected["metadata"] + sum(len(l.claims[n].ids) for n in l.retired),
              "metadata_conservation")
        check(expected["metadata"] <= l.cfg.metadata and expected["commands"] <= l.cfg.credits,
              "reservation_bounds")
        if command.startswith("read ") and rc == "OK":
            from decoder import decode
            seq = int(command.split()[1])
            check(wire is not None, "read_bytes_present")
            data = bytes.fromhex(wire)
            decode(data, l.claims[seq], l.cfg.pairs)
            check(seq not in self.blobs or data == self.blobs[seq], "retry_bytes_immutable")
            self.blobs[seq] = data
        else:
            check(wire is None, "no_unpublished_bytes")
        return rc

    def drain(self, seq, execute=True):
        if execute:
            for event in ("execute", "reports", "copy"):
                self.do(f"{event} {seq}", "OK")
            self.do(f"publish {seq} {seq}", "OK")
        self.do(f"read {seq}", "OK")
        self.do(f"ack {seq} {48 + len(self.ledger.claims[seq].ids) * 56}", "OK")

    def close(self):
        self.p.stdin.close()
        rc = self.p.wait(timeout=10)
        self.log.close()
        self.errors.close()
        if rc:
            raise RuntimeError(f"model exit {rc}; not a mutant kill")


def fences(r):
    r.reset(Config(pairs=1, ring_bytes=128, metadata=4))
    for i in range(1, 4):
        r.do(f"draw {i}", "OK", "admission_reserves_before_emission")
    r.do("execute 1", "OK")
    r.do("copy 1", "ORDER", "report_completion_fence")
    r.do("publish 1 1", "ORDER", "copy_publication_fence")
    r.do("reports 1", "OK")
    r.do("execute 2", "OK")
    r.do("execute 3", "LOCAL_BUSY", "local_read_lifetime")
    r.do("copy 1", "OK")
    r.do("execute 3", "OK", "reuse_without_cpu_label_observation")
    r.do("publish 1 0", "LABEL", "stale_label")
    r.do("publish 1 2", "LABEL", "future_label")
    r.do("publish 1 1", "OK")
    r.do("read 1", "OK", "destination_lifetime")
    r.do("ack 1 0", "SHORT", "short_sink_retains")
    r.do("ack 1 103", "SHORT", "short_sink_retains")
    r.do("ack 1 105", "SHORT", "short_sink_retains")
    r.do("read 1", "OK")
    r.do("ack 1 104", "OK")
    r.do("copy 1", "GENERATION", "retired_generation")


def capacity(r):
    r.reset(Config(pairs=1, ring_bytes=64, metadata=8))
    r.do("draw 1", "OK")
    r.do("draw 2", "OK")
    r.do("draw 3", "RING_FULL", "ring_full_is_visible")
    r.drain(1)
    r.do("draw 4", "OK", "ring_wrap_generation")
    r.do("publish 1 1", "GENERATION", "old_generation_after_wrap")
    r.drain(2)
    r.drain(3)
    r.do("stop", "OK")
    r.do("finish", "OK")
    for cfg, why in [(Config(metadata=0), "METADATA_FULL"), (Config(credits=0), "COMMAND_FULL")]:
        r.reset(cfg)
        r.do("draw 1", why, "reserve_before_emit")
        check(r.ledger.issued == 0, "no_leaked_empty_block")
    r.reset(Config(metadata=1))
    r.do("draw 1", "OK")
    r.do("draw 2", "METADATA_FULL")
    r.do("flush", "OK")
    r.drain(1)
    r.do("draw 3", "OK")
    r.reset(Config(pairs=1, ring_bytes=64, limit=2))
    for n in (1, 2):
        r.do(f"draw {n}", "OK")
        r.drain(n)
    r.do("draw 3", "SERIAL_FULL", "no_serial_wrap")


def allocation(r):
    cfg = Config()
    # Every attach allocation can fail. Refusal leaves no usable attachment;
    # the next successful attachment works. ASan checks partial cleanup.
    for nth in range(1, 7):
        command = cfg.command().replace("reset", "resetfail", 1) + f" {nth}"
        check(r.raw(command) == {"rc": "ALLOC"}, "allocation_refusal")
        check(r.raw("draw 1") == {"rc": "NO_ATTACH"}, "failed_attach_no_emission")
        r.reset(cfg)
        r.do("draw 1", "OK")
    for cfg in (Config(blocks=1), Config(pairs=0), Config(ring_bytes=1), Config(limit=0)):
        check(r.raw(cfg.command()) == {"rc": "CONFIG"}, "invalid_capacity_refused")
    r.reset()


def boundaries(r):
    r.reset(Config(ring_bytes=512, metadata=16, credits=8))
    r.do("draw 1", "OK")
    r.do("frame 1", "OK", "frame_tail_flush")
    r.do("draw 2", "OK")
    r.do("window 0", "OK", "window_tail_flush")
    r.do("draw 3", "WINDOW_CLOSED")
    r.do("window 2", "OK")
    r.do("draw 4", "OK")
    r.do("stop", "OK", "seal_tail_flush")
    r.do("finish", "INCOMPLETE", "bounded_seal_retains_pending")
    r.do("draw 5", "STOPPED")
    for n in range(1, 4):
        r.drain(n)
    r.do("finish", "OK")
    check([r.ledger.claims[n].ids for n in range(1, 4)] == [[1], [2], [4]], "boundary_identity")


def quarantine(r):
    r.reset()
    r.do("draw 1", "OK")
    r.do("uncertain 2", "QUARANTINE", "uncertain_emit_retains")
    r.do("execute 1", "ORDER")
    r.do("stop", "OK")
    r.do("finish", "INCOMPLETE")
    for phase in ("SEALED", "EXECUTING", "REPORTS", "COPIED", "READY", "READING"):
        r.reset(Config(pairs=1))
        r.do("draw 1", "OK")
        events = ["execute 1", "reports 1", "copy 1", "publish 1 1", "read 1"]
        while r.ledger.claims[1].state != phase:
            r.do(events.pop(0), "OK")
        r.do("fault 1", "QUARANTINE")
        r.do("ack 1 104", "ORDER")
        r.do("stop", "OK")
        r.do("finish", "INCOMPLETE")
    for retired in (False, True):
        r.reset(Config(pairs=1))
        r.do("draw 1", "OK")
        if retired:
            r.drain(1)
        r.do("replay 1", "REPLAY", "replay_is_not_new_work")
        r.do("draw 2", "REPLAY")
        r.do("stop", "OK")
        r.do("finish", "INCOMPLETE")


def decoder_cases(r):
    from decoder import decode, decode_stream
    r.reset(Config(pairs=2))
    for n in range(1, 5):
        r.do(f"draw {n}", "OK")
    for n in (1, 2):
        r.drain(n)
    good, expected = r.blobs[1], r.ledger.claims[1]
    def refused(blob, name):
        try:
            decode(blob, expected, 2)
        except ValueError:
            return
        raise AssertionError("decoder_" + name)
    for offset, value, fmt, name in [
        (0, b"BAD!", "4s", "magic"), (4, 2, "H", "version"),
        (6, 0, "H", "uncommitted"), (6, 3, "H", "unknown_flags"),
        (8, 2, "Q", "generation"), (16, 0, "I", "empty"),
        (16, 3, "I", "capacity"), (20, 32, "I", "record_width"),
        (24, 1, "I", "payload_length"), (28, 1, "I", "reserved"),
        (32, 2, "Q", "operation"), (40, 8, "Q", "owner"),
        (48, 1, "Q", "frame"), (56, 2, "Q", "window"),
        (64, 2, "Q", "record_generation"), (72, 99, "Q", "timestamp"),
        (88, 1, "Q", "duplicate_operation"), (len(good)-8, 0, "Q", "torn_commit")]:
        b = bytearray(good)
        struct.pack_into("<" + fmt, b, offset, value)
        refused(b, name)
    for b in (good[:-1], good + b"\0"):
        refused(b, "exact_length")
    decode_stream([r.blobs[1], r.blobs[2]], r.ledger.claims, 2)
    for blobs in ([r.blobs[1]], [r.blobs[1], r.blobs[1]], [r.blobs[2], r.blobs[1]]):
        try:
            decode_stream(blobs, r.ledger.claims, 2)
        except ValueError:
            continue
        raise AssertionError("decoder_stream_missing_duplicate_or_order")


def exhaustive(r):
    """All distinct states within seven steps, including refused transitions."""
    cfg = Config(pairs=1, ring_bytes=64, metadata=2, credits=2, limit=3)
    seen, stack, edges = set(), [([], Ledger(cfg))], 0
    while stack:
        trace, ledger = stack.pop()
        key = json.dumps(ledger.expected(), sort_keys=True)
        if (len(trace), key) in seen:
            continue
        seen.add((len(trace), key))
        if len(trace) == 7:
            continue
        choices = [f"draw {ledger.last_id+1}", "stop", "finish"]
        for c in ledger.live():
            choices.extend(f"{event} {c.seq}" for event in ("execute", "reports", "copy", "read"))
            choices += [f"publish {c.seq} {c.seq}", f"ack {c.seq} 104"]
        for event in choices:
            r.reset(cfg)
            for old in trace:
                r.do(old)
            r.do(event, name="exhaustive_ledger")
            edges += 1
            nxt = copy.deepcopy(ledger)
            nxt.step(event)
            stack.append((trace + [event], nxt))
    return dict(depth=7, states=len(seen), edges=edges)


def interleavings(r):
    """All linear extensions of two complete block lifetimes.

    Draw execution is ordered, and draining is ordered. Reports, transfers and
    per-generation publication may complete in either order between blocks.
    """
    events = ["execute", "reports", "copy", "publish", "read", "ack"]
    def orders(i=0, j=0, trace=()):
        if i == j == 6:
            yield trace
        if i < 6:
            yield from orders(i+1, j, trace+((1, events[i]),))
        if j < 6 and (j != 0 or i >= 1) and (j != 4 or i == 6):
            yield from orders(i, j+1, trace+((2, events[j]),))
    schedules = list(orders())
    for pairs in (1, 2):
        for trace in schedules:
            r.reset(Config(pairs=pairs, ring_bytes=2*pairs*32, metadata=2*pairs, credits=2))
            for ident in range(1, 2*pairs+1):
                r.do(f"draw {ident}", "OK")
            for seq, event in trace:
                suffix = f" {seq}" if event == "publish" else f" {48+pairs*56}" if event == "ack" else ""
                r.do(f"{event} {seq}{suffix}", "OK", "complete_interleaving")
            for ident in range(2*pairs+1, 3*pairs+1):
                r.do(f"draw {ident}", "OK")
            r.drain(3)
            r.do("stop", "OK")
            r.do("finish", "OK")
    return dict(schedules_per_capacity=len(schedules), capacities=2)


def large_blocks(r, out, ring_bytes, usable_pairs):
    rows = []
    for blocks in (2, 3, 4):
        pairs = usable_pairs // blocks
        capacity = blocks*pairs
        cfg = Config(blocks=blocks, pairs=pairs, ring_bytes=ring_bytes)
        cfg.metadata, cfg.credits = cfg.slots*pairs, cfg.slots
        r.reset(cfg)
        # Owner queues two full frames before execution: GPU one frame behind.
        for frame in range(2):
            for i in range(500):
                r.do(f"draw {frame*500+i+1}", "OK")
            r.do(f"frame {frame+1}", "OK")
        transfers = r.ledger.issued // 2
        check(transfers == (500+pairs-1)//pairs, "large_block_transfer_count")
        for seq in range(1, blocks+1):
            r.do(f"execute {seq}", "OK")
            r.do(f"reports {seq}", "OK")
        r.do(f"execute {blocks+1}", "LOCAL_BUSY", "delayed_copy_stalls_execution")
        for seq in range(1, r.ledger.issued+1):
            if seq > blocks:
                r.do(f"execute {seq}", "OK")
                r.do(f"reports {seq}", "OK")
            r.do(f"copy {seq}", "OK")
            r.do(f"publish {seq} {seq}", "OK")
            r.drain(seq, execute=False)
        # Queue a single frame far beyond the local pool before any device or
        # consumer progress. The ring and metadata, not local slots, bound it.
        for i in range(1001, 3001):
            r.do(f"draw {i}", "OK")
        r.do("stop", "OK")
        for c in list(r.ledger.live()):
            r.drain(c.seq)
        r.do("finish", "OK")
        # Fixed small ring: device completion does not release unread slots.
        small = Config(blocks=blocks, pairs=pairs, ring_bytes=blocks*pairs*32,
                       metadata=capacity+pairs, credits=blocks+1)
        r.reset(small)
        for seq in range(1, blocks+1):
            for j in range(pairs):
                r.do(f"draw {(seq-1)*pairs+j+1}", "OK")
            for event in ("execute", "reports", "copy"):
                r.do(f"{event} {seq}", "OK")
            r.do(f"publish {seq} {seq}", "OK")
        r.do(f"draw {capacity+1}", "RING_FULL", "fixed_ring_no_growth")
        # A ring sized for exactly one 500-draw frame is full after its
        # boundary flush, even when the last block is only partially used.
        one_frame = Config(blocks=blocks, pairs=pairs, ring_bytes=transfers*pairs*32,
                           metadata=transfers*pairs, credits=transfers)
        r.reset(one_frame)
        for ident in range(1, 501):
            r.do(f"draw {ident}", "OK")
        r.do("frame 1", "OK")
        for seq in range(1, transfers+1):
            for event in ("execute", "reports", "copy"):
                r.do(f"{event} {seq}", "OK")
            r.do(f"publish {seq} {seq}", "OK")
        r.do("draw 501", "RING_FULL", "frame_tail_occupies_slot")
        r.drain(1, execute=False)
        r.do("draw 502", "OK", "drainer_recovers_admission")
        full_frames, remaining = divmod(cfg.slots, transfers)
        paused_limit = full_frames*500 + min(500, remaining*pairs)
        rows.append(dict(blocks=blocks, pairs_per_block=pairs, usable_pairs=usable_pairs,
                         unused_pool_pairs=usable_pairs-capacity,
                         transfers_per_500_frame=transfers, admitted_two_queued_frames=1000,
                         first_execution_stall_generation=blocks+1,
                         fixed_small_ring_exclusion_visit=capacity+1,
                         one_frame_ring_exclusion_visit=501,
                         ring_bytes=cfg.ring_bytes, ring_slots=cfg.slots,
                         full_block_draw_capacity=cfg.slots*pairs,
                         predicted_paused_500_frame_exclusion_visit=paused_limit+1))
    (out / "large-blocks.json").write_text(json.dumps(rows, indent=2) + "\n")
    return rows


CASES = dict(fences=fences, capacity=capacity, allocation=allocation, boundaries=boundaries,
             quarantine=quarantine, decoder=decoder_cases)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--binary", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--case", choices=CASES)
    ap.add_argument("--ring-bytes", type=int, default=16*1024*1024)
    ap.add_argument("--usable-pairs", type=int, default=504)
    args = ap.parse_args()
    if not 256 <= args.usable_pairs <= 512 or not 65536 <= args.ring_bytes <= 64*1024*1024:
        ap.error("host scenario needs 256..512 usable pairs and 64 KiB..64 MiB ring bytes")
    args.out.mkdir(parents=True, exist_ok=True)
    r = Runner(args.binary, args.out)
    summary = {}
    try:
        for name, fn in CASES.items():
            if args.case and name != args.case:
                continue
            fn(r)
            print(name + " PASS", flush=True)
        if not args.case:
            summary["schedules"] = exhaustive(r)
            summary["interleavings"] = interleavings(r)
            summary["large_blocks"] = large_blocks(r, args.out, args.ring_bytes, args.usable_pairs)
        summary.update(steps=r.steps, resets=r.resets, result="PASS")
    except AssertionError as e:
        print("ASSERTION: " + str(e), flush=True)
        return 1
    finally:
        r.close()
    (args.out / "result.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
