import json, struct
d = json.load(open('docs/usb-research/evidence/inventory/ptpd.json'))
cmds = d['commands']
sizeofcmds = cmds[-1]['offset'] + cmds[-1]['size'] - 32
NAMES = {'LC_SEGMENT_64':0x19,'LC_DYLD_CHAINED_FIXUPS':0x80000034,'LC_DYLD_EXPORTS_TRIE':0x80000033,
 'LC_SYMTAB':0x02,'LC_DYSYMTAB':0x0b,'LC_LOAD_DYLINKER':0x0e,'LC_UUID':0x1b,'LC_BUILD_VERSION':0x32,
 'LC_SOURCE_VERSION':0x2a,'LC_MAIN':0x80000028,'LC_LOAD_DYLIB':0x0c,'LC_LOAD_WEAK_DYLIB':0x80000018,
 'LC_FUNCTION_STARTS':0x26,'LC_DATA_IN_CODE':0x29,'LC_CODE_SIGNATURE':0x1d}
out = bytearray()
out += struct.pack('<IiiIIIII', 0xfeedfacf, 0x100000c, -0x7ffffffe, d['filetype'], len(cmds), sizeofcmds, int(d['flags'],16), 0)
assert len(out) == 32
for c in cmds:
    body = bytearray(struct.pack('<II', NAMES[c['name']], c['size']) + bytes(c['size'] - 8))
    if c['name'] == 'LC_UUID':
        body[8:24] = bytes.fromhex(d['uuid'].replace('-',''))
    if c['name'] == 'LC_BUILD_VERSION':
        b = d['build']
        struct.pack_into('<IIII', body, 8, b['platform'], 17<<16, 17<<16, (c['size']-24)//8)
    out += body
open('/tmp/ptpd-header.bin','wb').write(bytes(out))
print(f'reconstructed header: {len(out)} bytes, {len(cmds)} commands, sizeofcmds {sizeofcmds}')
