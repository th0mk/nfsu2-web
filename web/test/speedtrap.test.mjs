import test from 'node:test';
import assert from 'node:assert/strict';
import * as esbuild from 'esbuild';
import * as P from '../src/net/proto.ts';
const code=(await esbuild.build({entryPoints:[new URL('../src/net/speedtrap.ts',import.meta.url).pathname],bundle:true,write:false,format:'esm'})).outputFiles[0].text;
const {trapResponse,trapRequest}=await import('data:text/javascript;base64,'+Buffer.from(code).toString('base64'));
const state={v:1,id:'airport',sequence:1,event:{id:'airport',name:'Airport approach',speed:2160,best:true},nearby:[{id:'airport',distance:40,bearing:90}],name:'Airport approach',active:true,distance:40,bearing:90,best:2160,last:2160,passes:1,session:'a',gate:[1695,-845,24.6,14],rows:[{name:'<img src=x>',speed:2160,me:true}]};
function response(s=state,src=P.NET_ROAM_HUB) {
 const body=new TextEncoder().encode('STP1'+JSON.stringify(s)),f=new Uint8Array(P.F_DATA+body.length);
 f[0]=P.NET_KIND_UDP;new DataView(f.buffer).setUint32(P.F_SRC,src);f.set(body,P.F_DATA);return f;
}
test('only hub results with a bounded valid schema are delivered',()=>{
 assert.deepEqual(trapResponse(response()),state);
 assert.equal(trapResponse(response(state,0x0a4d0002)),null);
 for(const invalid of [{...state,last:999999},{...state,nearby:Array(5).fill(state.nearby[0])},{...state,event:{...state.event,speed:-1}},{...state,sequence:-1},{...state,rows:Array(6).fill(state.rows[0])},{...state,active:1},{...state,distance:null},{...state,rows:[null]}]) assert.equal(trapResponse(response(invalid)),null);
 assert.equal(trapResponse(new Uint8Array(2)),null);
});
test('requests reverse the hub PEERS ports and ignore peer and non-PEERS traffic',()=>{
 const f=new Uint8Array(P.F_DATA+20),v=new DataView(f.buffer);
 f[0]=P.NET_KIND_UDP;v.setUint32(P.F_SRC,P.NET_ROAM_HUB);v.setUint32(P.F_DST,0x0a4d0002);
 v.setUint16(P.F_SPORT,47300);v.setUint16(P.F_DPORT,47210);f[P.F_DATA+5]=2;
 const r=trapRequest(f),d=new DataView(r.buffer);
 assert.equal(d.getUint32(P.F_SRC),0x0a4d0002);assert.equal(d.getUint32(P.F_DST),P.NET_ROAM_HUB);
 assert.equal(d.getUint16(P.F_SPORT),47210);assert.equal(d.getUint16(P.F_DPORT),47300);
 assert.equal(new TextDecoder().decode(r.subarray(P.F_DATA)),'STQ1');
 f[P.F_DATA+5]=3;assert.equal(trapRequest(f),null);
 f[P.F_DATA+5]=2;v.setUint32(P.F_SRC,0x0a4d0003);assert.equal(trapRequest(f),null);
});

test('shutter has two bounded transients and decays to silence', async()=>{
 const out=(await esbuild.build({entryPoints:[new URL('../src/audio/shutter.ts',import.meta.url).pathname],bundle:true,write:false,format:'esm'})).outputFiles[0].text;
 const {shutterSamples}=await import('data:text/javascript;base64,'+Buffer.from(out).toString('base64'));
 for(const rate of [44100,48000]) {
   const samples=shutterSamples(rate);
   const peak=(a,b)=>Math.max(...samples.slice(Math.floor(a*rate),Math.floor(b*rate)).map(Math.abs));
   assert.equal(samples.length,Math.ceil(rate*.18));
   assert.ok(samples.every(x=>Number.isFinite(x)&&Math.abs(x)<.3));
   assert.ok(peak(0,.02)>.08);assert.ok(peak(.065,.085)>peak(.045,.06)*3);
   assert.ok(peak(.16,.18)<.001);
 }
});
