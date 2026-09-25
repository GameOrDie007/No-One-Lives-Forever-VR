"""Rebuild every vertex from its weights and compare against the file's own
bind position. This is what proved the deformation.

  python tools/abc-verify-weights.py <model>.ABC [<model>.ABC ...]

Two arms. "old" is what the renderer shipped, sum_i w_i * (M_i . p_i). "new" is
sum_i (R_i . p_i + w_i * t_i), which is the same thing once you know the stored
offset is already scaled by the bias.

Run it from a directory holding the extracted models. The new arm lands within
0.0001 units on every model in the game; the old one misses by up to 17 on a
character 106 units tall, and only ever on vertices with more than one weight.
"""

import importlib.util, math, sys, glob, os, statistics
spec=importlib.util.spec_from_file_location("abcm","abc.py"); m=importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
spec2=importlib.util.spec_from_file_location("abcn","abcnodes.py"); mn=importlib.util.module_from_spec(spec2); spec2.loader.exec_module(mn)

def old(mm,q,b):   # what renstub does today: w * (M . p)
    return (b*(mm[0]*q[0]+mm[1]*q[1]+mm[2]*q[2]+mm[3]),
            b*(mm[4]*q[0]+mm[5]*q[1]+mm[6]*q[2]+mm[7]),
            b*(mm[8]*q[0]+mm[9]*q[1]+mm[10]*q[2]+mm[11]))
def new(mm,q,b):   # R . p  +  w * t     (p is already bias-scaled)
    return (mm[0]*q[0]+mm[1]*q[1]+mm[2]*q[2]+b*mm[3],
            mm[4]*q[0]+mm[5]*q[1]+mm[6]*q[2]+b*mm[7],
            mm[8]*q[0]+mm[9]*q[1]+mm[10]*q[2]+b*mm[11])

def run(path):
    d,secs,H,pieces=m.parse(path,verbose=False)
    nodes=mn.parse_nodes(d,secs,verbose=False)
    res={}
    for label,fn in (("old",old),("new",new)):
        errs=[]; multi=[]
        for P in pieces:
            for L in P['lods']:
                for v in L['verts']:
                    acc=[0,0,0]; ws=0
                    ok=True
                    for (ni,loc,b) in v['w']:
                        if ni>=len(nodes): ok=False; break
                        q=fn(nodes[ni]['mat'],loc,b)
                        acc=[acc[i]+q[i] for i in range(3)]; ws+=b
                    if not ok: continue
                    if abs(ws)>1e-6 and abs(ws-1)>1e-3: acc=[a/ws for a in acc]
                    e=math.dist(acc,v['loc']); errs.append(e)
                    if len(v['w'])>1: multi.append(e)
        res[label]=(len(errs), statistics.mean(errs), max(errs), (statistics.mean(multi) if multi else 0), (max(multi) if multi else 0), len(multi))
    return H, res

files = sys.argv[1:] or [r"nolfrez\CHARS\MODELS\HERO_ACTION.ABC"]
for f in files:
    H,res=run(f)
    o=res['old']; n=res['new']
    print(f"{os.path.basename(f):34s} verts {o[0]:6d} multi {o[5]:6d} | OLD mean {o[1]:7.4f} max {o[2]:7.3f} (multi mean {o[3]:7.4f} max {o[4]:7.3f}) | NEW mean {n[1]:.6f} max {n[2]:.6f}")
