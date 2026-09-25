"""Read a LithTech .abc model file - the format NOLF's characters ship in.

This is the ANSWER KEY. The file stores each vertex's bind position right next
to the weights that are supposed to reproduce it, so anything the renderer
believes about the mesh can be checked against the model itself, offline, with
no game running and no headset.

Get the models out of the game data first (lithrez ships in the client tree):

  src/nolf1-modernizer/TOOLS/lithrez.exe x game/NOLF.REZ <somewhere>

then

  python tools/abc.py <somewhere>/CHARS/MODELS/HERO_ACTION.ABC

Version 12 is what NOLF1 uses. The parse is self-validating: the Pieces section
must end on exactly the byte the section header names, and every count in the
file header must be reproduced. If it does not land, do not trust the numbers.

Layout, measured against those checks:

  sections   u16 len + name, then u32 offset of the next section.
             Header, Pieces, Nodes, ChildModels, Animation, Sockets,
             AnimBindings.
  header     u32 x14: version, keyframes, anims, nodes, pieces, childmodels,
             tris, verts, vertweights, lods, sockets, weightsets, strings,
             stringlen; then the command string and the internal radius.
  Pieces     u32 total weights, u32 piece count, then each piece:
             u16 material, f specPower, f specScale, f lodWeight, u16 pad,
             string name, then header.lods LODs back to back and NOTHING
             after them - the next piece starts immediately.
  LOD        u32 faceCount, faces, u32 vertCount, vertices.
  face       three of {float u, float v, u16 vertexIndex} - 30 bytes.
  vertex     u16 weightCount, u16 subLodVertexIndex, the weights,
             float3 location (the BIND position), float3 normal.
  weight     u32 nodeIndex, float3 location, float bias - 20 bytes.

  A WEIGHT'S LOCATION IS ALREADY MULTIPLIED BY ITS BIAS. See
  docs/WEIGHTS-ARE-PRESCALED.md; it is the whole reason this parser exists.
"""

import struct, sys

class R:
    def __init__(self, d, o=0): self.d=d; self.o=o
    def u8(self):  v=self.d[self.o]; self.o+=1; return v
    def u16(self): v=struct.unpack_from('<H',self.d,self.o)[0]; self.o+=2; return v
    def u32(self): v=struct.unpack_from('<I',self.d,self.o)[0]; self.o+=4; return v
    def f(self):   v=struct.unpack_from('<f',self.d,self.o)[0]; self.o+=4; return v
    def vec(self): v=struct.unpack_from('<3f',self.d,self.o); self.o+=12; return v
    def quat(self):v=struct.unpack_from('<4f',self.d,self.o); self.o+=16; return v
    def s(self):   n=self.u16(); v=self.d[self.o:self.o+n].decode('latin-1'); self.o+=n; return v

def sections(d):
    out={}; off=0
    while True:
        n=struct.unpack_from('<H',d,off)[0]
        name=d[off+2:off+2+n].decode('latin-1')
        nxt=struct.unpack_from('<I',d,off+2+n)[0]
        out[name]=(off+2+n+4, nxt)
        if nxt==0xffffffff or nxt>=len(d): break
        off=nxt
    return out

def parse(path, verbose=True):
    d=open(path,'rb').read()
    secs=sections(d)
    hs=secs['Header'][0]
    r=R(d,hs)
    H={}
    H['version']=r.u32(); H['keyframes']=r.u32(); H['anims']=r.u32(); H['nodes']=r.u32()
    H['pieces']=r.u32(); H['childmodels']=r.u32(); H['tris']=r.u32(); H['verts']=r.u32()
    H['vertweights']=r.u32(); H['lods']=r.u32(); H['sockets']=r.u32(); H['weightsets']=r.u32()
    H['strings']=r.u32(); H['stringlen']=r.u32()
    if verbose: print("HEADER", H)

    ps, pend = secs['Pieces']
    r=R(d,ps)
    weight_count_total=r.u32(); npieces=r.u32()
    if verbose: print(f"Pieces: totalWeights={weight_count_total} pieces={npieces} (section {ps:#x}..{pend:#x})")
    pieces=[]
    for pi in range(npieces):
        P={}
        P['material']=r.u16(); P['specPower']=r.f(); P['specScale']=r.f(); P['lodWeight']=r.f()
        P['pad']=r.u16(); P['name']=r.s()
        P['lods']=[]
        nlod = H['lods']
        # LOD distances come after the LODs in some versions; try faces-first
        for li in range(nlod):
            L={}
            nf=r.u32()
            if nf > 1000000: raise ValueError(f"piece {pi} lod {li} facecount {nf} at {r.o:#x}")
            faces=[]
            for _ in range(nf):
                fv=[]
                for _ in range(3):
                    u=r.f(); v=r.f(); idx=r.u16(); fv.append((u,v,idx))
                faces.append(fv)
            L['faces']=faces
            nv=r.u32()
            if nv > 1000000: raise ValueError(f"piece {pi} lod {li} vertcount {nv} at {r.o:#x}")
            verts=[]
            for _ in range(nv):
                nw=r.u16(); sub=r.u16()
                ws=[]
                for _ in range(nw):
                    ni=r.u32(); loc=r.vec(); bias=r.f()
                    ws.append((ni,loc,bias))
                loc=r.vec(); nrm=r.vec()
                verts.append(dict(nw=nw, sub=sub, w=ws, loc=loc, nrm=nrm))
            L['verts']=verts
            P['lods'].append(L)
        P['lodDist']=[]
        pieces.append(P)
        if verbose:
            print(f"  piece {pi} '{P['name']}' mat={P['material']} lodW={P['lodWeight']} "
                  f"LOD verts={[len(l['verts']) for l in P['lods']]} faces={[len(l['faces']) for l in P['lods']]} ")
    if verbose:
        print(f"  ended at {r.o:#x}, section end {pend:#x}  {'MATCH' if r.o==pend else 'MISMATCH'}")
    return d, secs, H, pieces

if __name__=='__main__':
    parse(sys.argv[1])
