"""The E1 probe's control flow as the design states it (r3 C/D, r4, review 10:25 point 5): for one fault script, the
EXACT marker list, the exact PPU call/cleanup inventory, the exact producer-thread inventory and the exit status.
Polling calls (p.drain, p.lane_stats, p.gpu_stats) are not in the PPU inventory; their count is bounded separately.
Imported by test_e1.py only.  Faults (a dict; absent keys = no fault):
  sink, init, tb, reg, lc: the call fails | setup: spu-init|import|group-create|thread-init | start: group start fails
  attach: attach fails | flush: k (the k-th writer flush fails) | badidx: k (an index fault latched after flush k)
  rep1, rep2: timeout|identity (stale/nocommit/torn) | drain: timeout|error|stats-error | mb: error|value
  dec: efault|eagain | fini: fails | term, join, lane_end, destroy, close: the call fails
  hb, gcm, gattach: fails | frame_fail: (k, what) with what in frame|begin|end|mark|stats|drain|timeout
  detach: timeout|error | release, shutdown: fails | cap: open|read
  log: NN (marker NN's own write fails: the stage-99 record follows, nothing new is submitted)"""

E1_FLUSHES_PER_EPOCH = 9  # 8 cadence flushes (>= 16 pending) + the epoch-end flush, both epochs
FRAMES = 30


def flow(F):
    marks, ppu, spu = [], [], []
    st = {'log': False}

    def mark(n):
        if st['log']:
            return
        if F.get('log') == n:
            st['log'] = True
            marks.append(99)
            return
        marks.append(n)

    def logged():
        return st['log']

    # ---- the producer (deterministic on its own thread) ----
    def producer(start_ok):
        if not start_ok:
            return
        spu.append('w.attach')
        if F.get('attach'):
            spu.append('report 1')
            return 'exited'
        k = 0
        for i in range(E1_FLUSHES_PER_EPOCH):
            if F.get('badidx') is not None and k == F['badidx']:
                spu.append('report 1')  # the latched fault is reported before the next flush is called
                return 'exited'
            k += 1
            spu.append(f'w.flush {k}')
            if F.get('flush') == k:
                spu.append('report 1')
                return 'exited'
        spu.append('report 1')
        if F.get('rep1') in ('timeout', 'identity'):
            return 'blocked' if F.get('rep1') == 'timeout' else 'mbox'
        return 'mbox'

    def producer_phase2():
        spu.append('mbox-read')
        if F.get('mb') == 'value':
            spu.append('report 2')
            return 'exited'
        k = E1_FLUSHES_PER_EPOCH + 1
        spu.append(f'w.flush {k}')
        if F.get('flush') == k:
            spu.append('report 2')
            return 'exited'
        spu.append('w.dec 1')
        if F.get('dec') == 'efault':
            spu.append('report 2')
            return 'exited'
        if F.get('dec') == 'eagain':
            k += 1
            spu.append(f'w.flush {k}')
            spu.append('w.dec 2')
        for i in range(E1_FLUSHES_PER_EPOCH):
            k += 1
            spu.append(f'w.flush {k}')
            if F.get('flush') == k:
                spu.append('report 2')
                return 'exited'
        spu.append('w.fini 1')
        spu.append('report 2')
        return 'exited'

    # ---- the PPU ----
    mark(1)
    ppu.append('p.set_sink_path')
    if F.get('sink'):
        mark(2)
        return marks, ppu, spu, 1
    ppu.append('p.init_ex')
    if F.get('init'):
        mark(2)
        return marks, ppu, spu, 1
    mark(2)
    failed = False
    ppu.append('p.thread_begin')
    if F.get('tb'):
        mark(3)
        failed = True
    else:
        mark(3)
        sp = {'lane': False, 'imported': False, 'group': False, 'started': False, 'exited': False, 'joined': False,
              'clean': False, 'ext': False}
        ok = True
        for step, key in (('p.register', 'reg'), ('p.lane_create', 'lc')):
            ppu.append(step)
            if F.get(key):
                ok = False
                break
        if ok:
            sp['lane'] = True
            for step in ('spu-init', 'import', 'group-create', 'thread-init'):
                ppu.append(step)
                if F.get('setup') == step:
                    ok = False
                    break
                if step == 'import':
                    sp['imported'] = True
                if step == 'group-create':
                    sp['group'] = True
        mark(4)
        pstate = None
        if not ok:
            failed = True
        elif not logged():
            mark(5)
            if not logged():
                ppu.append('group-start')
                if F.get('start'):
                    sp['started'] = 'unknown'
                    failed = True
                    mark(6)
                else:
                    sp['started'] = True
                    mark(6)
                    pstate = producer(True)
                    r1 = F.get('rep1')
                    if pstate == 'exited':  # an error report 1 (trusted)
                        sp['exited'] = True
                        failed = True
                        mark(7)
                    elif r1 in ('timeout', 'identity'):
                        failed = True
                        mark(7)
                    else:
                        mark(7)
                        mark(8)
                        d = F.get('drain')
                        if d:
                            failed = True
                            if d in ('error', 'stats-error'):
                                sp['ext'] = True
                            mark(9)
                        else:
                            mark(9)
                            if not logged():
                                mark(10)
                                if not logged():
                                    ppu.append('mb')
                                    if F.get('mb') == 'error':
                                        failed = True
                                        mark(11)
                                    else:
                                        mark(11)
                                        pstate = producer_phase2()
                                        r2 = F.get('rep2')
                                        if r2 in ('timeout', 'identity'):
                                            failed = True
                                            mark(12)
                                        else:
                                            sp['exited'] = True
                                            err = F.get('mb') == 'value' or F.get('dec') == 'efault' or F.get('fini') \
                                                or (F.get('flush') or 0) > E1_FLUSHES_PER_EPOCH
                                            if err:
                                                failed = True
                                            else:
                                                sp['clean'] = True
                                            mark(12)
        # SPU cleanup
        if sp['lane']:
            term_failed = False
            if sp['started']:
                if not sp['exited']:
                    ppu.append('terminate')
                    if F.get('term'):
                        term_failed = True
                        failed = True
                mark(13)
                if term_failed:
                    mark(14)
                else:
                    mark(14)
                    ppu.append('join')
                    if F.get('join'):
                        failed = True
                    else:
                        sp['joined'] = True
                    mark(15)
            else:
                mark(14)
            if sp['joined'] and not sp['ext']:
                ppu.append('p.lane_end')
                if F.get('lane_end'):
                    failed = True
                    sp['ext'] = True
                    sp['clean'] = False
            elif not sp['joined']:
                sp['clean'] = False
            mark(16)
            if sp['started'] and not sp['joined']:
                pass
            elif sp['group'] or sp['imported']:
                if sp['group']:
                    ppu.append('destroy')
                if F.get('destroy'):
                    failed = True
                elif sp['imported']:
                    ppu.append('image-close')
                    if F.get('close'):
                        failed = True
            mark(17)
        # GPU
        if sp['clean'] and not sp['ext'] and not logged():
            ppu.append('gcm host-buffer')
            if F.get('hb'):
                failed = True
                mark(18)
            else:
                ppu.append('gcm init')
                if F.get('gcm'):
                    failed = True
                    mark(18)
                else:
                    mark(18)
                    if not logged():
                        ppu.append('p.gpu_attach')
                        if F.get('gattach'):
                            failed = True
                            mark(19)
                        else:
                            mark(19)
                            mark(20)
                            frames, ext = 0, False
                            fk, fwhat = F.get('frame_fail', (None, None))
                            for fr in range(1, FRAMES + 1):
                                if logged() or ext:
                                    break
                                ppu.append('p.gpu_frame')
                                if fk == fr and fwhat == 'frame':
                                    break
                                ppu.append('p.gpu_begin')
                                if fk == fr and fwhat == 'begin':
                                    break
                                ppu.append('p.gpu_end')
                                if fk == fr and fwhat == 'end':
                                    break
                                ppu.append('p.gpu_mark')
                                if fk == fr and fwhat == 'mark':
                                    break
                                ppu.append('gcm-flush')
                                if fk == fr and fwhat in ('stats', 'drain'):
                                    ext = True
                                    break
                                if fk == fr and fwhat == 'timeout':
                                    break
                                frames += 1
                            if frames != FRAMES:
                                failed = True
                            mark(21)
                            if not logged() and not ext:
                                ppu.append('p.gpu_detach')
                                if F.get('detach'):
                                    failed = True
                                    mark(22)
                                else:
                                    mark(22)
                                    if frames == FRAMES:
                                        mark(23)
                                        ppu.append('gcm-finish')
                                        mark(24)
                                        ppu.append('p.gpu_release')
                                        if F.get('release'):
                                            failed = True
                                        mark(25)
    # shutdown
    mark(26)
    ppu.append('p.shutdown')
    if F.get('shutdown'):
        failed = True
        mark(27)
    else:
        mark(27)
        if F.get('cap'):
            failed = True
        mark(28)
    if logged():
        failed = True
    return marks, ppu, spu, 1 if failed else 0
