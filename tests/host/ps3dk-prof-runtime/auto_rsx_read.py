"""Independent reader for host-model artifacts; no production wire format."""
REASONS = ('observed', 'unsupported', 'context', 'reentry', 'size', 'reports',
           'metadata', 'room', 'emission')

def coverage(record):
    def integer(n, maximum=(1<<64)-1):
        if type(n) is not int or not 0<=n<=maximum:raise ValueError('invalid counter')
        return n
    lost=integer(record['coverage_lost'],1)
    frames=record['frames']
    if not isinstance(frames,list) or not 1<=len(frames)<=129:raise ValueError('frame inventory')
    seen=set();answer=[]
    for f in frames:
        owner=integer(f['owner'],(1<<32)-1);frame=integer(f['frame'])
        key=(owner,frame)
        if key in seen:raise ValueError('duplicate frame')
        seen.add(key)
        closed=integer(f['closed'],1);unsupported=integer(f['unsupported'])
        if len(f['kinds'])!=3:raise ValueError('kind inventory')
        visited=observed=excluded=room=0
        named=dict.fromkeys(REASONS[1:],0);kinds={}
        for kind,k in zip(('ARRAY','INDEX','CLEAR'),f['kinds']):
            v=integer(k['visited']);o=integer(k['observed']);e=k['excluded']
            if len(e)!=len(REASONS):raise ValueError('reason inventory')
            e=[integer(n) for n in e]
            if e[0] or (not lost and v!=o+sum(e)):raise ValueError('coverage does not conserve operations')
            visited+=v;observed+=o;excluded+=sum(e);room+=e[7]
            for name,n in zip(REASONS[1:],e[1:]):named[name]+=n
            kinds[kind]=dict(visited=v,observed=o,excluded=sum(e),reasons=dict(zip(REASONS[1:],e[1:])))
        answer.append(dict(owner=owner,frame=frame,visited=visited,observed=observed,
            excluded=excluded,reasons=named,kinds=kinds,unsupported=unsupported,
            status='INCOMPLETE' if lost or not closed or not owner else 'COVERED_POPULATION_ONLY',
            needs_room_plan=None if lost or not closed or not owner else bool(visited and room*100>=visited),
            direct_uninstrumented_population='UNKNOWN',timing_readiness='NOT_ESTABLISHED_BY_ADMISSION_COUNTS'))
    return answer

def projection(off, on, serial_base=1):
    def packets(words):
        i=0
        while i<len(words):
            h=words[i]
            if type(h) is not int or not 0<=h<=0xffffffff:raise ValueError('word')
            n=(h>>18)&2047
            if not n or h&0xa0030003 or i+n>=len(words):raise ValueError('packet boundary')
            yield words[i:i+n+1];i+=n+1
    # Decode packet boundaries independently of the sizing implementation.
    list(packets(off));list(packets(on))
    if len(on)!=len(off)+12:raise ValueError('whole-operation marker cost')
    for index,block in enumerate((on[:6],on[-6:])):
        if block[::2]!=[0x00041800,0x00041d6c,0x00041d70] or block[3]!=3840:
            raise ValueError('marker methods/label')
        if block[1] != 0x01000000+index*16:raise ValueError('report ownership')
        serial=serial_base+index
        swapped=(serial&0xff00ff00)|((serial>>16)&255)|((serial&255)<<16)
        if block[5] != swapped:raise ValueError('serial identity')
    if on[6:-6]!=off:raise ValueError('application command projection')
    return on[6:-6]
