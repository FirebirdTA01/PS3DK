"""Sequential stage 2 specification. Device facts are supplied explicitly.

This is not a target implementation or a memory-order proof. The separate
stage 1 C model remains the command projection and room-coverage authority.
"""

MAX_SERIAL = (1 << 32) - 1


class Capture:
    def __init__(self, pairs=504, independent_drain=True, foreign=(), frame_capacity=256, generation=1):
        self.pairs = pairs
        self.independent_drain = independent_drain
        self.foreign = list(foreign)
        self.first, self.count, self.op_label, self.clock_label = 1024, 1024, 239, 238
        self.state = 'NONE'
        self.leased = False
        self.label = self.latched = self.prepared = self.put_frontier = 0
        self.next_serial = 1
        self.configs = []
        self.override_calls = self.app_calls = self.marker_words = 0
        self.issued = self.written = 0
        self.tickets = {}
        self.exclusions = {}
        self.clocks = []
        self.generation = generation
        self.clock_prepared = self.clock_put_frontier = self.clock_device_label = 0
        self.config_frozen = False
        self.frame_capacity = frame_capacity
        self.frames = []
        self.frames_lost = 0
        self.last_frame = -1

    @property
    def pending(self):
        return sum(not t['ack'] for t in self.tickets.values())

    @property
    def held(self):
        return sum(not t['retired'] for t in self.tickets.values())

    def reason(self, text):
        self.configs.append((text, 1))
        return text

    @staticmethod
    def valid(first, count, op_label, clock_label):
        labels = {64, *range(69, 255)}
        return (0 <= first < 2048 and count >= 18 and count % 2 == 0 and
                first + count <= 2048 and op_label in labels and clock_label in labels and
                op_label != clock_label)

    def conflict(self, first, count, op_label, clock_label):
        return any((first < base + n and base < first + count) or label in (op_label, clock_label)
                   for base, n, label in self.foreign)

    def begin_init(self, *, clean, managed):
        if self.state == 'SEALED':
            return self.reason('STOPPED')
        if self.state != 'NONE':
            return self.reason('CONTEXT_ALREADY_CLAIMED')
        if not self.independent_drain:
            return self.reason('NO_INDEPENDENT_DRAIN')
        if not clean:
            return self.reason('PRIOR_WRITES_UNKNOWN')
        if not managed:
            return self.reason('RESOURCE_LEASE_UNVERIFIED')
        if self.conflict(self.first, self.count, self.op_label, self.clock_label):
            return self.reason('RESOURCE_CONFLICT')
        self.state = 'ATTACHING'
        return self.state

    def finish_init(self):
        if self.state != 'ATTACHING':
            # A failed claim never writes NONE over a concurrent terminal seal.
            return self.reason('STOPPED')
        self.leased = True
        self.label = 0  # only after the clean-context ownership precondition
        self.state = 'ATTACHED'
        return self.reason('ATTACHED')

    def gcm_init(self, *, clean, managed, application_rc=0):
        if application_rc:
            self.reason('GCM_INIT_FAILED')
            return application_rc
        result = self.begin_init(clean=clean, managed=managed)
        if result == 'ATTACHING':
            self.finish_init()
        return application_rc

    def override(self, *, first, count, op_label, clock_label):
        self.override_calls += 1
        if self.state != 'ATTACHED':
            return 'NOT_ATTACHED'
        if self.issued or self.clocks or self.config_frozen:
            return 'LIVE_LEASE'
        if not self.valid(first, count, op_label, clock_label):
            return 'BAD_OPTIONS'
        if self.conflict(first, count, op_label, clock_label):
            return 'RESOURCE_CONFLICT'
        self.first, self.count, self.op_label, self.clock_label = first, count, op_label, clock_label
        self.pairs = min(self.pairs, (count - 16) // 2)
        return 'ATTACHED'

    def config_accepted(self):
        self.config_frozen = True

    def exclude(self, reason):
        self.exclusions[reason] = self.exclusions.get(reason, 0) + 1
        return 0

    def submit(self):
        self.app_calls += 1
        if self.state != 'ATTACHED':
            return self.exclude('unattached')
        if self.next_serial > MAX_SERIAL - 2:
            return self.exclude('serial')
        if self.held >= self.pairs:
            return self.exclude('reports')
        self.issued += 1
        self.tickets[self.issued] = dict(begin=self.next_serial, end=self.next_serial + 1,
                                        raw=None, complete=False, ack=False, retired=False, quarantined=False)
        self.next_serial += 2
        self.marker_words += 12
        self.config_frozen = True
        return self.issued

    def prepare_publication(self, serial):
        if self.state != 'ATTACHED' or not self.prepared <= serial < self.next_serial:
            return False
        self.prepared = serial
        return True

    def put(self, serial):
        if self.state != 'ATTACHED' or not self.put_frontier <= serial <= self.prepared:
            return False
        self.put_frontier = serial
        return True

    def device(self, serial, reports):
        # Explicit fake-device event, not inferred from CPU label memory alone.
        if not self.label <= serial <= self.put_frontier:
            return False
        for ident, raw in reports.items():
            t = self.tickets.get(ident)
            if t is None or t['end'] > serial or t['retired'] or t['raw'] is not None:
                return False
        for ident, raw in reports.items():
            self.tickets[ident]['raw'] = raw
        self.label = serial
        return True

    def observe(self, serial, publication_snapshot, stable=True):
        if self.state != 'ATTACHED':
            return 'STOPPED'
        if serial >= self.next_serial or serial < self.latched:
            return 'INVALID_LABEL'
        if serial > publication_snapshot or serial != self.label or serial > self.put_frontier:
            return 'DEFER'
        if not stable:
            return 'READ_REFUSED'
        reached = [t for t in self.tickets.values() if t['end'] <= serial and not t['retired']]
        if not reached or any(t['raw'] is None for t in reached):
            return 'DEFER'
        if any(t['raw'][0] > t['raw'][1] for t in reached):
            for t in reached:
                t['quarantined'] = True
            return 'CLOCK_REGRESSION'
        self.latched = serial  # latch before any slot is allowed to retire
        for t in reached:
            if not t['quarantined']:
                t['complete'] = True
        return 'COMPLETE'

    def complete_ids(self):
        return [i for i, t in self.tickets.items() if t['complete']]

    def sink(self, ident, *, whole):
        t = self.tickets[ident]
        if not whole or not t['complete'] or t['quarantined'] or t['ack']:
            return False
        t['ack'] = t['retired'] = True
        self.written += 1
        return True

    def sink_incomplete(self, ident, *, whole):
        t = self.tickets[ident]
        if not whole or not t['quarantined'] or t['ack']:
            return False
        t['ack'] = True
        self.written += 1
        return True  # metadata disposition does not retire its report slots

    def clock_issue(self):
        if self.state != 'ATTACHED' or len(self.clocks) >= 16:
            return None
        slot = self.first + len(self.clocks)
        self.clocks.append(dict(raw=None, ack=False))
        self.config_frozen = True
        return slot

    def clock_prepare(self, serial):
        if self.state != 'ATTACHED' or not self.clock_prepared <= serial <= len(self.clocks):
            return False
        self.clock_prepared = serial
        return True

    def clock_put(self, serial):
        if self.state != 'ATTACHED' or not self.clock_put_frontier <= serial <= self.clock_prepared:
            return False
        self.clock_put_frontier = serial
        return True

    def clock_device(self, serial, reports, *, generation=1):
        # The generation is supplied by the model trace, not encoded in a hardware
        # label. Physical reuse still requires a separately qualified handoff.
        if (self.state != 'ATTACHED' or generation != self.generation or
                not self.clock_device_label <= serial <= self.clock_put_frontier):
            return False
        if any(not 1 <= ident <= serial or self.clocks[ident - 1]['raw'] is not None for ident in reports):
            return False
        for ident, raw in reports.items():
            self.clocks[ident - 1]['raw'] = raw
        self.clock_device_label = serial
        return True

    def clock_complete(self, serial, *, stable, whole, publication_snapshot=0):
        if (self.state != 'ATTACHED' or not 1 <= serial <= len(self.clocks) or not stable or not whole or
                serial > publication_snapshot or serial > self.clock_put_frontier or
                serial > self.clock_device_label):
            return False
        t = self.clocks[serial - 1]
        if t['raw'] is None or t['ack']:
            return False
        t['ack'] = True
        return True

    def invalidate(self, reason):
        self.seal()
        self.reason(reason)

    def seal(self):
        self.state = 'SEALED'
        for t in self.tickets.values():
            if not t['retired']:
                t['quarantined'] = True

    def teardown_safe(self):
        # This predicate is necessary only; real cross-context teardown is a target gate.
        return self.held == 0 and self.pending == 0 and all(t['ack'] for t in self.clocks)

    def frame_close(self, ident, *, visited, observed, excluded):
        if ident <= self.last_frame or min(visited, observed, excluded) < 0 or visited != observed + excluded:
            return False
        self.last_frame = ident  # allocation frontier includes lost snapshots
        if len(self.frames) >= self.frame_capacity:
            self.frames_lost += 1
            return False
        self.frames.append((ident, visited, observed, excluded))
        return True

    def frame_offer(self):
        return self.frames[0] if self.frames else None

    def frame_ack(self, *, whole):
        if not whole or not self.frames:
            return False
        self.frames.pop(0)
        return True
