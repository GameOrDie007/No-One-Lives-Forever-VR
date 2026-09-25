"""Read the Nodes section of a .abc: the skeleton and its BIND pose.

  python tools/abcnodes.py <model>.ABC

A node is {string name, u16 index, u8 flags, float[16] matrix, u32 childCount},
written depth first, so the hierarchy falls out of the recursion. The matrix is
row-major with the translation in the fourth COLUMN - m[3], m[7], m[11] - and it
is the node's GLOBAL bind transform, not a local one: a NOLF character's feet
come out at y = -48 and its head at y = +39.

Check that before trusting anything built on it. Reading it the other way round
leaves every bone origin exactly right and rotates every vertex the wrong way,
which passes nearly every test this project owns.
"""

import struct, importlib.util, sys
spec=importlib.util.spec_from_file_location("abcm","abc.py"); m=importlib.util.module_from_spec(spec); spec.loader.exec_module(m)

def parse_nodes(d, secs, verbose=True):
    ns,nend=secs['Nodes']
    r=m.R(d,ns)
    nodes=[]
    def rec(parent):
        name=r.s(); idx=r.u16(); flags=r.u8()
        mat=struct.unpack_from('<16f', d, r.o); r.o+=64
        nch=r.u32()
        n=dict(name=name, idx=idx, flags=flags, mat=mat, parent=parent, children=[])
        nodes.append(n)
        for _ in range(nch):
            c=rec(idx); n['children'].append(c)
        return n
    root=rec(-1)
    if verbose:
        print(f"nodes parsed {len(nodes)}, ended {r.o:#x} section end {nend:#x} {'MATCH' if r.o==nend else 'MISMATCH'}")
    nodes.sort(key=lambda n:n['idx'])
    return nodes

if __name__=='__main__':
    p=sys.argv[1] if len(sys.argv)>1 else r"nolfrez\CHARS\MODELS\HERO_ACTION.ABC"
    d=open(p,'rb').read(); secs=m.sections(d)
    nodes=parse_nodes(d,secs)
    for n in nodes:
        mm=n['mat']
        print(f"{n['idx']:3d} parent={n['parent']:3d} flags={n['flags']} {n['name'][:28]:28s} "
              f"row3=({mm[3]:8.3f} {mm[7]:8.3f} {mm[11]:8.3f}) last4=({mm[12]:.3f} {mm[13]:.3f} {mm[14]:.3f} {mm[15]:.3f})")
