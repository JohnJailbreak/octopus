import struct, pathlib, json, uuid, hashlib

CMD={1:'LC_SEGMENT',2:'LC_SYMTAB',11:'LC_DYSYMTAB',12:'LC_LOAD_DYLIB',14:'LC_LOAD_DYLINKER',25:'LC_SEGMENT_64',27:'LC_UUID',29:'LC_CODE_SIGNATURE',33:'LC_ENCRYPTION_INFO',0x80000022:'LC_DYLD_INFO_ONLY',0x24:'LC_VERSION_MIN_MACOSX',0x25:'LC_VERSION_MIN_IPHONEOS',0x26:'LC_FUNCTION_STARTS',0x29:'LC_DATA_IN_CODE',0x2c:'LC_ENCRYPTION_INFO_64',0x32:'LC_BUILD_VERSION',0x80000028:'LC_MAIN',0x80000033:'LC_DYLD_EXPORTS_TRIE',0x80000034:'LC_DYLD_CHAINED_FIXUPS',0x2a:'LC_SOURCE_VERSION',0x80000018:'LC_LOAD_WEAK_DYLIB',0x8000001f:'LC_REEXPORT_DYLIB'}
def version(v): return '.'.join(str(x) for x in [v>>16,(v>>8)&255,v&255])
def parse(p):
 b=p.read_bytes(); u=lambda fmt,off:struct.unpack_from('<'+fmt,b,off); cs=lambda off:b[off:b.index(b'\0',off)].decode('utf8','replace')
 magic,cpu,sub,typ,ncmd,size,flags,res=u('8I',0); r=dict(name=p.name,size=len(b),sha256=hashlib.sha256(b).hexdigest(),cpu=hex(cpu),cpu_subtype=hex(sub),architecture='arm64e' if sub&0xffffff==2 else 'unknown',filetype=typ,flags=hex(flags),commands=[],segments=[],dylibs=[],sections=[],symbols=[]); off=32
 for _ in range(ncmd):
  cmd,sz=u('II',off); d=dict(name=CMD.get(cmd,hex(cmd)),offset=off,size=sz)
  if cmd==27:r['uuid']=str(uuid.UUID(bytes=b[off+8:off+24])).upper()
  if cmd==25:
   seg,vm,vs,fo,fs,maxp,initp,ns,fl=u('16sQQQQiiII',off+8); seg=seg.rstrip(b'\0').decode(); r['segments'].append(dict(name=seg,vmaddr=hex(vm),vmsize=vs,fileoff=fo,filesize=fs))
   for i in range(ns):
    sname,sseg,addr,ss,so,al,ro,rn,sflags,r1,r2,r3=u('16s16sQQIIIIIIII',off+72+i*80);r['sections'].append(dict(name=sname.rstrip(b'\0').decode(),segment=seg,address=hex(addr),size=ss,offset=so))
  elif cmd in (12,0x80000018,0x8000001f):r['dylibs'].append(cs(off+u('I',off+8)[0]))
  elif cmd==0x32:
   plat,minos,sdk,nt=u('IIII',off+8);r['build']=dict(platform=plat,minos=version(minos),sdk=version(sdk))
  elif cmd==2:
   so,n,st,ss=u('4I',off+8);r['symtab']=dict(nsyms=n,offset=so,string_size=ss)
   for i in range(n):
    ix,t,se,de,v=u('IBBHQ',so+i*16);r['symbols'].append(dict(name=cs(st+ix),type=t,section=se,value=hex(v)))
  elif cmd==29:
   co,cz=u('II',off+8);sig=dict(offset=co,size=cz,blobs=[]);magic,total,count=struct.unpack_from('>III',b,co)
   for i in range(count):
    slot,bo=struct.unpack_from('>II',b,co+12+i*8);bm,bl=struct.unpack_from('>II',b,co+bo);entry=dict(slot=slot,magic=hex(bm),length=bl)
    if bm==0xfade0c02:
     vers,sf,ho,io,ns,nc,cl=struct.unpack_from('>7I',b,co+bo+8);hs,ht,pl,ps=struct.unpack_from('4B',b,co+bo+36);entry.update(version=hex(vers),flags=hex(sf),identifier=cs(co+bo+io),code_limit=cl,hash_type=ht,hash_size=hs,page_log2=ps,code_slots=nc,special_slots=ns)
     if ht==2 and hs==32:
      entry['code_page_hashes_match']=all(hashlib.sha256(b[i*(1<<ps):min((i+1)*(1<<ps),cl)]).digest()==b[co+bo+ho+i*hs:co+bo+ho+(i+1)*hs] for i in range(nc))
    if bm==0xfade7171:entry['xml']=b[co+bo+8:co+bo+bl].decode('utf8','replace')
    sig['blobs'].append(entry)
   r['signature']=sig
  elif cmd==0x2c:r['encryption']=dict(zip(['offset','size','id'],u('III',off+8)))
  elif cmd==0x80000028:r['entryoff']=u('Q',off+8)[0]
  r['commands'].append(d); off+=sz
 return r
if __name__=='__main__':
 import sys
 out=pathlib.Path(sys.argv[2]);out.mkdir(parents=True,exist_ok=True)
 for p in pathlib.Path(sys.argv[1]).iterdir():
  if p.is_file():
   r=parse(p);(out/(p.name+'.json')).write_text(json.dumps(r,indent=2),encoding='utf8');print(p.name,r['uuid'],r.get('build'),r.get('symtab'))
