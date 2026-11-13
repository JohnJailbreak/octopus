import struct
from macho_parse import DATA, segments, sections, to_off, u32, u64, symtab, dysymtab

# ---- names: imported stubs, and selector references ----
strtab_off, strtab_size = symtab[2], symtab[3]
nsyms, symoff = symtab[1], symtab[0]
def sym_name(i):
    n = u32(symoff + i*16)
    e = DATA.index(b'\0', strtab_off + n)
    return DATA[strtab_off+n:e].decode(errors='replace')

indirect_off, indirect_n = dysymtab[2], dysymtab[3]
def indirect(i): return u32(indirect_off + i*4)

stub_names = {}
for sg, sn, addr, size, off, reloff, nreloc in sections:
    if sn in ('__auth_stubs','__stubs'):
        for k in range(size // 12):
            idx = indirect(reloff + k)
            if idx < nsyms: stub_names[addr + k*12] = sym_name(idx)
    if sn in ('__auth_got','__got'):
        for k in range(size // 8):
            idx = indirect(reloff + k)
            if idx < nsyms: stub_names[addr + k*8] = sym_name(idx)

selrefs = {}
for sg, sn, addr, size, off, *_ in sections:
    if sn == '__objc_selrefs':
        for k in range(size // 8):
            p = u64(off + k*8) & 0xffffffffff
            o = to_off(p)
            if o is not None:
                e = DATA.index(b'\0', o)
                selrefs[addr + k*8] = DATA[o:e].decode(errors='replace')

def cstring_at(va):
    o = to_off(va)
    if o is None: return None
    e = DATA.index(b'\0', o)
    s = DATA[o:e]
    return s.decode(errors='replace') if len(s) < 200 else None

R = lambda n: ('xzr' if n == 31 else f'x{n}')
W = lambda n: ('wzr' if n == 31 else f'w{n}')
COND = ['eq','ne','hs','lo','mi','pl','vs','vc','hi','ls','ge','lt','gt','le','al','nv']

def sx(v, bits):
    return v - (1 << bits) if v & (1 << (bits-1)) else v

def decode(w, pc):
    """A readable form for the arm64 subset Apple's ARC/ObjC code is made of."""
    # branches
    if w >> 26 == 0b000101: return f"b      {pc + sx(w & 0x3ffffff, 26)*4:#x}"
    if w >> 26 == 0b100101: return f"bl     {pc + sx(w & 0x3ffffff, 26)*4:#x}"
    if w & 0xfffffc1f == 0xd61f0000: return f"br     {R((w>>5)&31)}"
    if w & 0xfffffc1f == 0xd63f0000: return f"blr    {R((w>>5)&31)}"
    if w in (0xd65f03c0, 0xd65f0fff): return "ret"
    if w == 0xd50323bf: return "autiasp"
    if w == 0xd503233f: return "paciasp"
    if w == 0xd503237f: return "pacibsp"
    if w == 0xd65f0fff: return "retab"
    if w == 0xd503201f: return "nop"
    if w & 0xff000010 == 0x54000000:
        return f"b.{COND[w & 15]:<4} {pc + sx((w>>5)&0x7ffff, 19)*4:#x}"
    if w & 0x7f000000 in (0x34000000, 0x35000000):
        sf = 'x' if w >> 31 else 'w'
        op = 'cbz' if (w >> 24) & 1 == 0 else 'cbnz'
        return f"{op:<6} {sf}{w&31}, {pc + sx((w>>5)&0x7ffff, 19)*4:#x}"
    if w & 0x7e000000 == 0x36000000:
        op = 'tbz' if (w >> 24) & 1 == 0 else 'tbnz'
        bit = ((w >> 31) << 5) | ((w >> 19) & 31)
        return f"{op:<6} x{w&31}, #{bit}, {pc + sx((w>>5)&0x3fff, 14)*4:#x}"
    # adrp / add
    if w & 0x9f000000 == 0x90000000:
        imm = (((w >> 5) & 0x7ffff) << 2) | ((w >> 29) & 3)
        return f"adrp   {R(w&31)}, {((pc & ~0xfff) + sx(imm, 21)*0x1000):#x}"
    if w & 0xff800000 == 0x91000000:
        return f"add    {R(w&31)}, {R((w>>5)&31)}, #{(w>>10)&0xfff:#x}"
    if w & 0xff800000 == 0xd1000000:
        return f"sub    {R(w&31)}, {R((w>>5)&31)}, #{(w>>10)&0xfff:#x}"
    if w & 0xff800000 == 0x11000000:
        return f"add    {W(w&31)}, {W((w>>5)&31)}, #{(w>>10)&0xfff:#x}"
    # mov (register / immediate)
    if w & 0x7fe0ffe0 == 0x2a0003e0:
        return f"mov    {'x' if w>>31 else 'w'}{w&31}, {('x' if w>>31 else 'w')}{(w>>16)&31}"
    if w & 0x7f800000 == 0x52800000:
        return f"mov    {W(w&31)}, #{((w>>5)&0xffff) << (((w>>21)&3)*16):#x}"
    if w & 0x7f800000 == 0xd2800000:
        return f"mov    {R(w&31)}, #{((w>>5)&0xffff) << (((w>>21)&3)*16):#x}"
    if w & 0x7f800000 == 0x12800000:
        return f"mov    {W(w&31)}, #{~(((w>>5)&0xffff) << (((w>>21)&3)*16)) & 0xffffffff:#x}"
    # load/store pair
    if w & 0x7fc00000 in (0x29400000, 0x29000000, 0x28c00000, 0x28800000, 0xa9400000, 0xa9000000, 0xa8c00000, 0xa8800000):
        sf = 8 if w >> 31 else 4
        load = (w >> 22) & 1
        imm = sx((w >> 15) & 0x7f, 7) * sf
        return f"{'ldp' if load else 'stp':<6} {R(w&31) if sf==8 else W(w&31)}, {R((w>>10)&31) if sf==8 else W((w>>10)&31)}, [{R((w>>5)&31)}, #{imm:#x}]"
    # load/store immediate unsigned offset
    if w & 0x3b000000 == 0x39000000:
        size = w >> 30
        load = (w >> 22) & 1
        scale = 1 << size
        imm = ((w >> 10) & 0xfff) * scale
        reg = R(w & 31) if size == 3 else W(w & 31)
        op = ('ldr' if load else 'str') if size >= 2 else (('ldrb','strb') if size == 0 else ('ldrh','strh'))[not load]
        return f"{op:<6} {reg}, [{R((w>>5)&31)}, #{imm:#x}]"
    # ldr literal
    if w & 0x3b000000 == 0x18000000 and (w >> 24) & 3 == 0:
        return f"ldr    {R(w&31)}, {pc + sx((w>>5)&0x7ffff, 19)*4:#x}"
    if w & 0xffc00000 == 0xf9400000:
        return f"ldr    {R(w&31)}, [{R((w>>5)&31)}, #{((w>>10)&0xfff)*8:#x}]"
    if w & 0xffc00000 == 0xf9000000:
        return f"str    {R(w&31)}, [{R((w>>5)&31)}, #{((w>>10)&0xfff)*8:#x}]"
    # cmp / subs immediate
    if w & 0x7f80001f == 0x7100001f:
        return f"cmp    {W((w>>5)&31)}, #{(w>>10)&0xfff:#x}"
    if w & 0x7f80001f == 0xf100001f:
        return f"cmp    {R((w>>5)&31)}, #{(w>>10)&0xfff:#x}"
    if w & 0x7fe0fc1f == 0x6b00001f:
        return f"cmp    {W((w>>5)&31)}, {W((w>>16)&31)}"
    if w & 0x7fe0fc1f == 0xeb00001f:
        return f"cmp    {R((w>>5)&31)}, {R((w>>16)&31)}"
    if w & 0xffe0fc00 == 0x9a800000:
        return f"csel   {R(w&31)}, {R((w>>5)&31)}, {R((w>>16)&31)}, {COND[(w>>12)&15]}"
    if w & 0xffe0fc00 == 0x1a800000:
        return f"csel   {W(w&31)}, {W((w>>5)&31)}, {W((w>>16)&31)}, {COND[(w>>12)&15]}"
    if w & 0xffe0fc00 == 0x9a800400:
        return f"csinc  {R(w&31)}, {R((w>>5)&31)}, {R((w>>16)&31)}, {COND[(w>>12)&15]}"
    if w & 0xffe0fc00 == 0x1a800400:
        return f"cset/csinc {W(w&31)}, {W((w>>5)&31)}, {W((w>>16)&31)}, {COND[(w>>12)&15]}"
    if w == 0xd503309f or w == 0xd50339bf: return "dsb/isb"
    return f".word  {w:#010x}"

def disassemble(start, length, note=''):
    print(f"\n===== {start:#x} ({length} bytes) {note} =====")
    o = to_off(start)
    pending = {}
    for i in range(length // 4):
        pc = start + i*4
        w = struct.unpack_from('<I', DATA, o + i*4)[0]
        text = decode(w, pc)
        extra = ''
        # name a bl/blr target, and resolve adrp+add/ldr into selectors and strings
        if text.startswith('bl '):
            t = int(text.split()[-1], 16)
            if t in stub_names: extra = f"   ; {stub_names[t]}"
        if text.startswith('adrp'):
            pending[text.split(',')[0].split()[-1]] = int(text.split(', ')[-1], 16)
        if text.startswith(('add ', 'ldr ')) and ', #' in text:
            base = text.split('[')[-1].split(',')[0] if '[' in text else text.split(', ')[1]
            if base in pending:
                va = pending[base] + int(text.split('#')[-1].rstrip(']'), 16)
                if va in selrefs: extra = f'   ; @selector({selrefs[va]})'
                elif va in stub_names: extra = f'   ; {stub_names[va]}'
                else:
                    s = cstring_at(va)
                    if s and s.isprintable(): extra = f'   ; "{s}"'
                    else: extra = f'   ; {va:#x}'
        print(f"{pc:#x}  {text}{extra}")
