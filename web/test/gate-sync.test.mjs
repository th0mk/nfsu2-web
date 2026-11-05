import {test} from 'node:test';
import assert from 'node:assert/strict';
import * as esbuild from 'esbuild';
const code=(await esbuild.build({entryPoints:[new URL('../src/test/gate-sync.ts',import.meta.url).pathname],bundle:true,write:false,format:'esm'})).outputFiles[0].text;
const {attach}=await import('data:text/javascript;base64,'+Buffer.from(code).toString('base64'));
const wait=async fn=>{for(let i=0;i<200&&!fn();i++)await new Promise(r=>setTimeout(r,10));assert.ok(fn());};
test('startup, connected clients, later joins, deletes and exit synchronize without editor UI',async()=>{
 const oldFetch=globalThis.fetch;globalThis.window=new EventTarget();let points=[[10,20]],requests=0;
 globalThis.fetch=async()=>{requests++;return new Response(JSON.stringify(points));};
 const clients=[];
 const client=ready=>{let end;const calls=[];const state={ready,calls,end:()=>end()};const stop=attach({input:{photo(...args){if(!state.ready)return false;calls.push(args);return true;}},exited:new Promise(r=>{end=r;})});clients.push(stop);return state;};
 try{
  const a=client(true),b=client(false);await wait(()=>a.calls.length===3);assert.deepEqual(a.calls,[[33,0,1000],[34,0,2000],[35,1]]);
  b.ready=true;window.dispatchEvent(new Event('gate-catalog-saved'));await wait(()=>b.calls.length===3);
  points=[[30,40],[50,60]];await wait(()=>a.calls.at(-1)?.[1]===2&&b.calls.at(-1)?.[1]===2);
  const c=client(true);await wait(()=>c.calls.length===5);assert.deepEqual(c.calls[0],[33,0,3000]);
  points=[];window.dispatchEvent(new Event('gate-catalog-saved'));await wait(()=>clients.length===3&&[a,b,c].every(x=>x.calls.at(-1)?.[1]===0));
  a.end();b.end();c.end();await new Promise(r=>setTimeout(r,30));const count=requests;await new Promise(r=>setTimeout(r,1100));assert.equal(requests,count);
 }finally{clients.forEach(s=>s());globalThis.fetch=oldFetch;delete globalThis.window;}
});
