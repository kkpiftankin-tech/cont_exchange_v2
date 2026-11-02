# Воспроизведение измерения ADR-076: полная сетка переводов против звезды вокруг дома.
# Запуск: python3 docs/experiments/mesh_vs_tree.py (чистый Python, без зависимостей).
# Независимая реализация ClearCe: минимум Φ(x)=Σ ½α((|a-(x_u-x_v)|-c)+)² по свободным узлам.
# Решаем полу-гладким Ньютоном (активное множество) на чистом Python, как в ce_agent_clearing.hpp.
import random, math, statistics
def solve_lin(A,b):
    n=len(b); M=[row[:]+[b[i]] for i,row in enumerate(A)]
    for i in range(n): M[i][i]+=1e-12
    for c in range(n):
        p=max(range(c,n),key=lambda r:abs(M[r][c])); M[c],M[p]=M[p],M[c]
        for r in range(c+1,n):
            m=M[r][c]/M[c][c]
            for k in range(c,n+1): M[r][k]-=m*M[c][k]
    y=[0.0]*n
    for r in range(n-1,-1,-1): y[r]=(M[r][n]-sum(M[r][k]*y[k] for k in range(r+1,n)))/M[r][r]
    return y
def clear(free,E):
    idx={n:i for i,n in enumerate(free)}; F=len(free)
    X=lambda x,n:x[idx[n]] if n in idx else 0.0
    def flow(x,e):
        u,v,a,al,c=e; d=a-(X(x,u)-X(x,v)); g=abs(d)-c
        return al*(1 if d>0 else -1)*g if g>0 else 0.0
    def phi(x):
        s=0
        for (u,v,a,al,c) in E:
            d=a-(X(x,u)-X(x,v)); g=abs(d)-c
            if g>0: s+=0.5*al*g*g
        return s
    x=[0.0]*F
    for _ in range(200):
        g=[0.0]*F
        for e in E:
            f=flow(x,e)
            if e[0] in idx: g[idx[e[0]]]-=f
            if e[1] in idx: g[idx[e[1]]]+=f
        if max(abs(t) for t in g)<1e-13: break
        H=[[0.0]*F for _ in range(F)]
        for e in E:
            u,v,a,al,c=e; d=a-(X(x,u)-X(x,v))
            if abs(d)-c<=0: continue
            if u in idx: H[idx[u]][idx[u]]+=al
            if v in idx: H[idx[v]][idx[v]]+=al
            if u in idx and v in idx: H[idx[u]][idx[v]]-=al; H[idx[v]][idx[u]]-=al
        st=solve_lin(H,[-t for t in g]); p0=phi(x); t=1.0
        for _ in range(40):
            xn=[x[i]+t*st[i] for i in range(F)]
            if phi(xn)<=p0+1e-18: break
            t*=0.5
        x=xn
    return x,[flow(x,e) for e in E],idx

V=["binance","okx","kraken","coinbase"]; A=["BTC","ETH","SOL"]; HOUSE="binance"
def build(topology, anchors):
    nodes=[f"{a}@{v}" for a in A+["USDT"] for v in V]
    free=[n for n in nodes if n!="USDT@"+HOUSE]
    E=[]
    for v in V:
        for a in A: E.append((f"{a}@{v}",f"USDT@{v}",anchors[(a,v)],3000.0,0.0))   # переводчик a/USDT (глубокие книги)
    for a in A+["USDT"]:
        al,c=(12.0,0.25) if a!="USDT" else (18.0,0.05)
        if topology=="mesh": pairs=[(V[i],V[j]) for i in range(4) for j in range(i+1,4)]
        else: pairs=[(HOUSE,v) for v in V if v!=HOUSE]            # звезда вокруг дома
        for (x,y) in pairs: E.append((f"{a}@{x}",f"{a}@{y}",0.0,al,c))
    return free,E
random.seed(7); res={"mesh":[], "star":[]}; act={"mesh":[], "star":[]}; N=300
for _ in range(N):
    anchors={}
    for a in A:
        base=random.gauss(0,0.05)
        for v in V: anchors[(a,v)]=base+random.gauss(0,0.22/2**0.5)   # разность двух бирж ⇒ σ_diff=0.22‰
    for top in ("mesh","star"):
        free,E=build(top,anchors); x,f,idx=clear(free,E)
        tr=sum(abs(f[i]) for i,e in enumerate(E) if e[3] in (12.0,18.0))   # оборот переводов (тыс.USDT)
        res[top].append(tr); act[top].append(sum(1 for i,e in enumerate(E) if e[3] in (12.0,18.0) and abs(f[i])>1e-9))
for top in ("mesh","star"):
    n=len(res[top]); print("%s: рёбер переводов %d | оборот переводов за такт: медиана %.3f, среднее %.3f тыс.USDT | активных рёбер: среднее %.1f | такты без потока: %.0f%%"%(top,len([1 for e in build(top,{(a,v):0 for a in A for v in V})[1] if e[3] in (12.0,18.0)]),statistics.median(res[top]),statistics.mean(res[top]),statistics.mean(act[top]),100*sum(1 for r in res[top] if r<1e-9)/n))
print("отношение среднего оборота mesh/star: %.2f"%(statistics.mean(res["mesh"])/statistics.mean(res["star"])))
