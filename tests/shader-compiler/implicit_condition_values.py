"""Bounded execution of the condition fixtures, not a general RSX emulator.

Only unpredicated MOV/MUL/ADD/MAD/MAX/SNE/SEQ with literal blocks and TEX0
are accepted. Exact binary inputs exercise zero, sign and fractional truth.
Half temporaries are rounded on writes; unsupported instructions fail closed.
The two reference FP ternaries use condition-code selection instead: their
listings are independent oracle evidence, not instructions executed here.
"""
import math
import struct
from fp_sources import ARITY, CONST, INPUT, TEMP, instructions, source, ucode_words
from fp_vecmatmul_check import require, f32


def execute(blob, vector):
    regs={}
    ended=False
    for w,constant in instructions(ucode_words(blob)):
        ended=bool(w[0]&1)
        op=(w[0]>>24)&63
        if op in (0,0x3e):
            if ended: break
            continue
        require(op in (1,2,3,4,9,14,15),f'unsupported condition opcode {op}')
        require(((w[1]>>18)&7)==7,'predicated instruction outside this value checker')
        require(not (w[2]&(15<<28)),'unsupported scale/branch')
        require(not (w[0]&(3<<30)),'unsupported destination modifier')
        require(((w[0]>>22)&3) in (0,1),'unsupported precision')
        args=[]
        for slot in range(1,ARITY[op]+1):
            s=source(w,slot)
            if s['type']==INPUT:
                require(s['name']=='TEX0','unexpected input')
                data=vector
            elif s['type']==CONST:
                require(constant is not None,'missing literal block')
                data=[struct.unpack('>f',struct.pack('>I',x))[0] for x in constant]
            else:
                key=(s['half'],s['reg'])
                require(s['type']==TEMP and key in regs,'uninitialized temporary')
                data=regs[key]
            lanes=[data[(s['swizzle']>>(2*i))&3] for i in range(4)]
            if s['abs']: lanes=[abs(x) for x in lanes]
            if s['negate']: lanes=[-x for x in lanes]
            args.append(lanes)
        result=[]
        for lane in range(4):
            x=args[0][lane]
            if op in (2,4): x=f32(x*args[1][lane])
            if op==3: x=f32(x+args[1][lane])
            if op==4: x=f32(x+args[2][lane])
            if op==9: x=max(x,args[1][lane])
            if op==14: x=float(x!=args[1][lane])
            if op==15: x=float(x==args[1][lane])
            result.append(x)
        half=(w[0]>>7)&1
        if half: result=[struct.unpack('>e',struct.pack('>e',x))[0] for x in result]
        dst=(half,(w[0]>>1)&63);mask=(w[0]>>9)&15
        old=regs.setdefault(dst,[math.nan]*4)
        for lane in range(4):
            if mask&(1<<lane):old[lane]=result[lane]
        if ended:break
    require(ended,'missing END')
    require((0,0) in regs and all(math.isfinite(x) for x in regs[(0,0)]),'invalid colour output')
    return regs[(0,0)]


def verify(blob,route,name):
    for x,y in ((0.0,0.0),(-0.0,0.5),(0.5,0.0),(-0.5,0.5),(2.0,-2.0),(-2.0,0.0)):
        truth=x!=0
        if route in ('if','ternary'): expected=[1.,0.,0.,1.] if truth else [0.,0.,1.,1.]
        else:
            result=(not truth) if route=='not' else (truth and y!=0) if route=='and' else (truth or y!=0)
            expected=[float(result),0.,0.,1.]
        actual=execute(blob,[x,y,0.25,1.0])
        require(actual==expected,f'{name}: decoded value {actual} != {expected} at {x},{y}')
    # A wrong numerical expectation must disagree on the fractional cell.
    require(execute(blob,[0.5,0.5,0.25,1.0])!=[0.5,0.,0.5,1.],name+': inert value witness')
