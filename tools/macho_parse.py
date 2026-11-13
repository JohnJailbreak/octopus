import struct, sys
DATA = open(__import__('os').environ.get('OCT_MACHO','/mnt/user-data/uploads/ptpd'),'rb').read()

def u32(o): return struct.unpack_from('<I', DATA, o)[0]
def u64(o): return struct.unpack_from('<Q', DATA, o)[0]

magic = u32(0)
assert magic == 0xfeedfacf, hex(magic)
ncmds = u32(16); off = 32
segments, sections, symtab, dysymtab = [], [], None, None
for _ in range(ncmds):
    cmd, size = u32(off), u32(off+4)
    if cmd == 0x19:  # LC_SEGMENT_64
        name = DATA[off+8:off+24].rstrip(b'\0').decode()
        vm, vs, fo, fs = u64(off+24), u64(off+32), u64(off+40), u64(off+48)
        nsects = u32(off+64)
        segments.append((name, vm, vs, fo, fs))
        so = off + 72
        for _ in range(nsects):
            sn = DATA[so:so+16].rstrip(b'\0').decode()
            sg = DATA[so+16:so+32].rstrip(b'\0').decode()
            sections.append((sg, sn, u64(so+32), u64(so+40), u32(so+48), u32(so+56), u32(so+60)))
            so += 80
    elif cmd == 0x2:   symtab = (u32(off+8), u32(off+12), u32(off+16), u32(off+20))
    elif cmd == 0xb:   dysymtab = (u32(off+32), u32(off+36), u32(off+56), u32(off+60))
    off += size

def to_off(va):
    for name, vm, vs, fo, fs in segments:
        if vm <= va < vm + vs: return fo + (va - vm)
    return None

if __name__ == '__main__':
    print('segments:')
    for n, vm, vs, fo, fs in segments: print(f'  {n:<12} vaddr {vm:#x} size {vs:#x} fileoff {fo:#x}')
    print('sections of interest:')
    for sg, sn, a, s, o, *_ in sections:
        if sn in ('__text','__stubs','__auth_stubs','__objc_selrefs','__objc_methname','__cstring','__objc_classrefs','__got','__auth_got','__data'):
            print(f'  {sg},{sn:<18} addr {a:#x} size {s:#x} off {o:#x}')
    print('symtab:', symtab, 'dysymtab:', dysymtab)
    print('handleData: 0x100023998 -> file offset', hex(to_off(0x100023998)))
