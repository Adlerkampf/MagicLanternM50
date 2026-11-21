import random
def build(src,dst,rowb,rows,spitch):
    P=bytearray()
    def mov(rd,v): P.extend([0xBC,rd,v&255,(v>>8)&255,(v>>16)&255,(v>>24)&255])
    ccr=lambda l:(1|(3<<1)|((l-1)<<4)|(1<<14)|(3<<15)|((l-1)<<18)|0x1C00700)
    nb=rowb//128; rem=(rowb%128)//8; gap=spitch-rowb
    mov(0,src); mov(2,dst); left=rows
    while left:
        n=min(256,left); left-=n
        P.extend([0x22,n-1]); l1=len(P)
        mov(1,ccr(16)); P.extend([0x20,nb-1]); l0=len(P)
        P.extend([4,8]); P.extend([0x38,len(P)-l0])
        if rem: mov(1,ccr(rem)); P.extend([4,8])
        P.extend([0x54,gap&255,gap>>8])
        assert len(P)-l1<=255
        P.extend([0x3C,len(P)-l1])
    P.extend([0x13,0x34,0,0]); return P
def run(P,mem):
    pc=0; sar=dar=ccr=0; lc=[0,0]; fifo=b''
    while True:
        op=P[pc]
        if op==0xBC:
            v=int.from_bytes(P[pc+2:pc+6],'little'); r=P[pc+1]
            if r==0: sar=v
            elif r==1: ccr=v
            else: dar=v
            pc+=6
        elif op in (0x20,0x22): lc[(op>>1)&1]=P[pc+1]; pc+=2
        elif op==4:
            sz=1<<((ccr>>1)&7); ln=((ccr>>4)&15)+1; n=sz*ln
            fifo=bytes(mem[sar:sar+n]); sar+=n; pc+=1
        elif op==8:
            mem[dar:dar+len(fifo)]=fifo; dar+=len(fifo); pc+=1
        elif op in (0x38,0x3C):
            i=(op>>2)&1
            if lc[i]: lc[i]-=1; pc-=P[pc+1]
            else: pc+=2
        elif op==0x54: sar+=P[pc+1]|(P[pc+2]<<8); pc+=3
        elif op==0x13: pc+=1
        elif op==0x34: pc+=2
        elif op==0: return
        else: raise Exception(hex(op))
for cw,ch in ((2560,1440),(3584,1504),(3072,1728),(2880,1200)):
    pitch=7644; rowb=cw*14//8; x0b=((264+(4368-264-cw)//2)&~63)*14//8; y0=36
    srcsz=2202*pitch; mem=bytearray(random.randbytes(srcsz))+bytearray(rowb*ch+64)
    src=y0*pitch+x0b; dst=srcsz
    P=build(src,dst,rowb,ch,pitch); run(P,mem)
    ok=all(mem[dst+r*rowb:dst+(r+1)*rowb]==mem[src+r*pitch:src+r*pitch+rowb] for r in range(ch))
    print(cw,ch,'prog',len(P),'bytes','OK' if ok else 'FAIL', 'tail end clean', mem[dst+rowb*ch:dst+rowb*ch+64]==bytearray(64))
