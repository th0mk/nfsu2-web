import {test} from 'node:test';
import assert from 'node:assert/strict';
import * as esbuild from 'esbuild';
const code=(await esbuild.build({entryPoints:[new URL('../src/test/traps/save.ts',import.meta.url).pathname],bundle:true,write:false,format:'esm'})).outputFiles[0].text;
const {CatalogSave,CatalogConflict}=await import('data:text/javascript;base64,'+Buffer.from(code).toString('base64'));
function server(){
 let disk='original',version=1,lose=false,fail=false;
 const writes=[];
 return {writes,get disk(){return disk;},external(text){disk=text;version++;},lose(){lose=true;},fail(){fail=true;},
 async request(url,options={}){
  if(options.method==='PUT'){
   if(options.headers['If-Match']!==`"${version}"`)return new Response('',{status:412});
   if(fail){fail=false;return new Response('',{status:503});}
   disk=options.body;version++;writes.push(disk);
   if(lose){lose=false;throw Error('Response lost');}
  }
  return new Response(disk,{headers:{etag:`"${version}"`}});
 }};
}
test('lost successful reply is reconciled before newer edits are saved',async()=>{
 const s=server(),w=new CatalogSave('/catalog','"1"','original',s.request);
 s.lose();await assert.rejects(w.write('first'));await w.write('later');
 assert.equal(s.disk,'later');assert.deepEqual(s.writes,['first','later']);
});
test('retrying the same committed write does not duplicate it',async()=>{
 const s=server(),w=new CatalogSave('/catalog','"1"','original',s.request);
 s.lose();await assert.rejects(w.write('first'));await w.write('first');assert.deepEqual(s.writes,['first']);
});
test('uncommitted failures retry; genuine external changes remain protected',async()=>{
 const s=server(),w=new CatalogSave('/catalog','"1"','original',s.request);
 s.fail();await assert.rejects(w.write('first'));await w.write('later');assert.equal(s.disk,'later');
 s.lose();await assert.rejects(w.write('uncertain'));s.external('external');
 await assert.rejects(w.write('newer'),CatalogConflict);assert.equal(s.disk,'external');
});

test('reverting to the last acknowledged contents still reconciles an uncertain write',async()=>{
 const s=server(),w=new CatalogSave('/catalog','"1"','original',s.request);
 s.lose();await assert.rejects(w.write('first'));assert.equal(w.pending,true);
 await w.write('original');assert.equal(w.pending,false);assert.equal(s.disk,'original');
});
