"use strict";
// ---- ядро демонстрационного расчёта (копия блока //==CORE== из вложенного файла, без изменений логики) ----
const NODES=["BTC@Bin","ETH@Bin","SOL@Bin","BTC@OKX","ETH@OKX","SOL@OKX"];
const EDGES=[
 {id:"BTC/ETH@Bin",f:0,t:1,link:false},{id:"ETH/SOL@Bin",f:1,t:2,link:false},{id:"BTC/SOL@Bin",f:0,t:2,link:false},
 {id:"BTC/ETH@OKX",f:3,t:4,link:false},{id:"ETH/SOL@OKX",f:4,t:5,link:false},{id:"BTC/SOL@OKX",f:3,t:5,link:false},
 {id:"связка BTC",f:0,t:3,link:true},{id:"связка ETH",f:1,t:4,link:true},{id:"связка SOL",f:2,t:5,link:true}];
function solve(A,b){
  const n=b.length,M=A.map((r,i)=>r.concat([b[i]]));
  for(let c=0;c<n;c++){let p=c;for(let r=c+1;r<n;r++)if(Math.abs(M[r][c])>Math.abs(M[p][c]))p=r;
    [M[c],M[p]]=[M[p],M[c]]; if(Math.abs(M[c][c])<1e-14)return null;
    for(let r=0;r<n;r++){if(r===c)continue;const k=M[r][c]/M[c][c];for(let j=c;j<=n;j++)M[r][j]-=k*M[c][j];}}
  return M.map((r,i)=>r[n]/r[i]);
}
function compute(p){
  const R={};
  R.admitted = p.ageB<=p.tstale && p.ageO<=p.tstale;
  R.aext = Math.min(p.D1/1,p.D3/3,p.D6/6);
  R.aT_cap = p.WT/p.riskT;
  R.aT_extB = p.theta*p.fB*R.aext; R.aT_extO = p.theta*p.fO*R.aext;
  R.aTB = Math.min(R.aT_extB,R.aT_cap); R.aTO = Math.min(R.aT_extO,R.aT_cap);
  R.aA = p.WA/p.riskA;
  const alpha = [R.aTB,R.aTB,R.aTB,R.aTO,R.aTO,R.aTO,R.aA,R.aA,R.aA];
  R.alpha=alpha;
  const B=NODES.map(()=>EDGES.map(()=>0));
  EDGES.forEach((e,i)=>{B[e.f][i]=1;B[e.t][i]=-1;});
  const s=EDGES.map((e,i)=> e.link?0:Math.log(p.quotes[i]));
  const L=NODES.map((_,u)=>NODES.map((_,v)=>{let z=0;for(let i=0;i<9;i++)z+=B[u][i]*alpha[i]*B[v][i];return z;}));
  const beta=NODES.map((_,u)=>{let z=0;for(let i=0;i<9;i++)z+=B[u][i]*alpha[i]*s[i];return z;});
  R.B=B;R.s=s;R.L=L.map(r=>r.slice());R.beta=beta.slice();
  const g=5,xg=Math.log(200);
  const A2=L.map(r=>r.slice()),b2=beta.slice();
  for(let u=0;u<6;u++){b2[u]-=A2[u][g]*xg;}
  for(let u=0;u<6;u++){A2[u][g]=0;A2[g][u]=0;} A2[g][g]=1;b2[g]=xg;
  const x=solve(A2,b2); R.x=x;
  R.phi=x.map(Math.exp);
  R.basis=[0,1,2].map(k=>1000*(x[k]-x[k+3]));
  R.sigma=EDGES.map((e,i)=>x[e.f]-x[e.t]);
  R.w=EDGES.map((e,i)=>alpha[i]*(s[i]-R.sigma[i]));
  R.Bw=NODES.map((_,u)=>{let z=0;for(let i=0;i<9;i++)z+=B[u][i]*R.w[i];return z;});
  R.nett=Math.max(...R.Bw.map(Math.abs));
  R.ledger=EDGES.map((e,i)=>{const row=Array(6).fill(0);
    row[e.f]= R.w[i]/R.phi[e.f]; row[e.t]=-R.w[i]/R.phi[e.t]; return row;});
  R.colSum=NODES.map((_,u)=>R.ledger.reduce((z,r)=>z+r[u],0));
  R.surplus=EDGES.map((e,i)=>1000*R.w[i]*R.w[i]/(2*alpha[i]));
  R.surplusTot=R.surplus.reduce((a,b)=>a+b,0);
  return R;
}
module.exports = { compute, solve, NODES, EDGES };
