import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import * as esbuild from 'esbuild';
const code=(await esbuild.build({entryPoints:[new URL('../src/test/traps/model.ts',import.meta.url).pathname],bundle:true,write:false,format:'esm'})).outputFiles[0].text;
const {parseCatalog,catalog,crossing,surfaces,readRoads}=await import('data:text/javascript;base64,'+Buffer.from(code).toString('base64'));
const source=fs.readFileSync(new URL('../server-rs/crates/roam/src/speedtraps.def',import.meta.url),'utf8');
test('shared catalog can be edited and round-tripped without losing IDs or endpoint heights',()=>{
 const gates=parseCatalog(source);gates.push({id:'bridge',a:[-40,250,100],b:[0,250,102]});
 assert.deepEqual(parseCatalog(catalog(gates)),gates);
 for(const text of ['TRAP(a, ,0,0,10,0,0)','TRAP(a, NaN,0,0,10,0,0)','TRAP(a, 0,0,0,1,0,0)','TRAP(UPPER, 0,0,0,10,0,0)',catalog(gates)+catalog(gates)])assert.throws(()=>parseCatalog(text));
 assert.throws(()=>catalog(Array.from({length:65},(_,i)=>({id:`t${i}`,a:[0,0,0],b:[20,0,0]}))));
});
test('crossing respects finite gate ends, interpolated elevation and both directions',()=>{
 const g={id:'sloped',a:[-10,0,10],b:[10,0,14]};
 assert.deepEqual(crossing(g,[0,-20,12],[0,20,12]),{heightError:0,direction:1});
 assert.deepEqual(crossing(g,[0,20,12],[0,-20,12]),{heightError:0,direction:2});
 assert.equal(crossing(g,[11,-20,12],[11,20,12]),null);
 assert.equal(crossing(g,[0,-20,12],[0,-1,12]),null);
 assert.equal(crossing(g,[0,-20,40],[0,20,40]).heightError,28);
});
test('overlapping roads expose separate heights instead of silently choosing a bridge',()=>{
 const roads=[{id:1,points:[[-100,0,0],[100,0,10]]},{id:2,points:[[-100,0,50],[100,0,60]]}];
 const result=surfaces(roads,0,8);
 assert.deepEqual(result.map(s=>[s.road,s.z,s.distance]),[[1,5,8],[2,55,8]]);
});
test('road reader checks native table aliases, bounds and node heights',()=>{
 const buffer=new ArrayBuffer(32*1024*1024),d=new DataView(buffer),road=0x1000000;
 d.setUint32(0x88a334,road,true);d.setUint32(0x88a338,road,true); // duplicate pointer, not road 1
 d.setUint32(0x88a33c,0x7fffffff,true); // invalid span
 d.setInt16(road+0xa,0,true);d.setInt16(road+0x34,2,true);
 for(let i=0;i<2;i++){const p=road+0x8c+i*0x38;d.setFloat32(p,10+i*30,true);d.setFloat32(p+4,-80,true);d.setInt16(p+0x12,200+i*8,true);}
 assert.deepEqual(readRoads(buffer),[{id:0,points:[[10,-80,25],[40,-80,26]]}]);
});
test('editor is a separate dev bundle and production build excludes it',()=>{
 const root=new URL('../dist/',import.meta.url),dev=new URL('../dist-dev/test/trap-editor.js',import.meta.url);
 assert.ok(fs.existsSync(dev));assert.ok(!fs.existsSync(new URL('test/trap-editor.js',root)));
 assert.ok(!fs.readFileSync(new URL('main.js',root),'utf8').includes('0x88a334'));
});

const projectionCode=(await esbuild.build({entryPoints:[new URL('../src/test/traps/projection.ts',import.meta.url).pathname],bundle:true,write:false,format:'esm'})).outputFiles[0].text;
const {project,unproject}=await import('data:text/javascript;base64,'+Buffer.from(projectionCode).toString('base64'));
test('rendered geometry projection round-trips clicks at different road heights',()=>{
 const m=[2,0,0,.001,0,3,0,.002,0,0,1,-1,-10,20,0,100];
 for(const point of [[10,-30,0],[-20,40,20],[5,8,60]]){
  const [x,y]=project(m,point,1440,1000),actual=unproject(m,x,y,point[2],1440,1000);
  actual.forEach((v,i)=>assert.ok(Math.abs(v-point[i])<1e-8));
 }
 assert.throws(()=>unproject(Array(16).fill(0),100,100,0,1440,1000));
});
