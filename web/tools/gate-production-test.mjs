// Run against an actual server WITHOUT --dev, built from the current catalog.
// Ordinary game/settings and keyboard input; only the approach position is set by the harness.
// No editor, dev endpoints, test scripts, marker overrides, or submitted scores.
import {chromium} from 'playwright';
import {pickBrowser} from './browser.mjs';
import fs from 'node:fs';
import assert from 'node:assert/strict';
import * as esbuild from 'esbuild';
const base=process.argv[2]||'http://localhost:8082',out=process.argv[3]||'out/web/gate-production';fs.mkdirSync(out,{recursive:true});
const model=(await esbuild.build({entryPoints:[new URL('../src/test/traps/model.ts',import.meta.url).pathname],bundle:true,write:false,format:'iife',globalName:'GateTestModel'})).outputFiles[0].text;
const catalog=fs.readFileSync(new URL('../server-rs/crates/roam/src/speedtraps.def',import.meta.url),'utf8');
const registry=fs.readFileSync(new URL('../../tests/web/seed/registry_perf4_native.txt',import.meta.url),'utf8');
const browser=await chromium.launch({...pickBrowser(),headless:true}),page=await browser.newPage({viewport:{width:1280,height:900}}),logs=[],scripts=[];
page.on('console',m=>{logs.push(m.text());fs.appendFileSync(`${out}/live.log`,m.text()+'\n');});page.on('request',r=>{if(r.resourceType()==='script')scripts.push(r.url());});
const frames=async n=>{const start=await page.evaluate(()=>window.__nfsu2.session.status.read()?.get('frames')??0);await page.waitForFunction(({start,n})=>(window.__nfsu2.session.status.read()?.get('frames')??0)>=start+n,{start,n},{timeout:60000});};
const tap=async key=>{await page.keyboard.press(key,{delay:35});};
const seen=s=>logs.some(x=>x.includes(s));
const waitLog=async(s,ms=60000)=>{const end=Date.now()+ms;while(!seen(s)&&Date.now()<end)await page.waitForTimeout(100);assert.ok(seen(s),`Missing game screen: ${s}`);};
try{
 await page.goto(base+'/');
 for(const path of ['/test/dev.json','/test/gates/catalog','/test/gates/markers','/test/trap-editor.js','/test/gate-sync.js'])assert.equal((await page.request.get(base+path)).status(),404,path);
 await page.evaluate(async registry=>{const root=await navigator.storage.getDirectory(),save=await root.getDirectoryHandle('save',{create:true});for(const [name,text] of [['settings.ini','movies = off\nroam = on\nroam_name = GateProd\nroam_room = production-gate-test\n'],['registry.txt',registry]]){const f=await save.getFileHandle(name,{create:true}),w=await f.createWritable();await w.write(text);await w.close();}},registry);
 await page.reload();await page.locator('#play').click();await page.waitForFunction(()=>document.body.dataset.phase==='playing',null,{timeout:90000});
 assert.equal(await page.evaluate(()=>window.__nfsu2.env().NFSU2_GATE_EDITOR),undefined);
 for(let i=0;i<40&&!seen('screen MC_Background.fng');i++){await tap('Enter');await page.waitForTimeout(1500);}
 await waitLog('screen MC_Background.fng');await frames(120);await tap('ArrowLeft');await frames(30);await tap('Enter');await frames(240);
 for(let i=0;i<4;i++){await tap('Backspace');await frames(8);}await frames(20);
 await page.evaluate(()=>window.__nfsu2.session.input.text(Array.from('GateProd',c=>c.charCodeAt(0))));
 await frames(60);await tap('Enter');await frames(240);await tap('Enter');await waitLog('screen GarageMain.fng');
 await frames(300);await tap('ArrowLeft');await frames(60);await tap('Enter');await frames(300);await tap('Enter');
 await waitLog('screen UI_EngageMessageDialog.fng');await frames(120);await tap('Enter');
 await page.waitForFunction(()=>(window.__nfsu2.session.status.read()?.get('drive')??0)%4===2,null,{timeout:60000});await frames(1000);
 await page.keyboard.press('F9');await page.locator('#menu').waitFor({state:'visible'});assert.equal(await page.locator('#menu-dev').isVisible(),false);await page.getByRole('button',{name:'Back to the game',exact:true}).click();
 await page.evaluate(model+';globalThis.GateTestModel=GateTestModel;');
 const gates=await page.evaluate(text=>GateTestModel.parseCatalog(text),catalog);
 const checkMarkers=kind=>{for(let i=0;i<gates.length;i++){
  const prefix=`[speedtrap] ${kind} camera ${i} at `,line=logs.find(x=>x.includes(prefix));assert.ok(line,`compiled ${kind} gate ${i}`);
  const xy=line.slice(line.indexOf(prefix)+prefix.length).split(' ').map(Number);
  for(let axis=0;axis<2;axis++)assert.ok(Math.abs(xy[axis]-(gates[i].a[axis]+gates[i].b[axis])/2)<0.11,`${kind} saved gate ${i} coordinate ${axis}`);
 }};checkMarkers('minimap');
 const upright=async kind=>{
  const pins=await page.evaluate(({kind,count})=>{
   const d=new DataView(window.__nfsu2.session.proc.memory.buffer),u=p=>d.getUint32(p,true);
   const hash=s=>{let h=0xffffffff;for(const c of s)h=(Math.imul(h,33)+c.charCodeAt(0))>>>0;return h;};
   const wanted=new Set(Array.from({length:count},(_,i)=>hash(`${kind==='minimap'?'TRAP_CAMERA_':'TRAP_MAP_'}${i}`))),found=[];
   const add=o=>{if(o&&wanted.has(u(o+0x10))){const data=u(o+0x2c);found.push(Array.from({length:4},(_,i)=>d.getFloat32(data+0x28+i*4,true)));}};
   if(kind==='minimap'){const head=u(0x8364d4);for(let n=u(head),i=0;n&&n!==head&&i<512;n=u(n),i++){const v=u(n+0x1c);if(v)add(u(v));}}
   else {const pk=u(u(u(0x8384c4)+8)+0xe4);for(let o=u(pk+0x4c),i=0;o&&i<8192;o=u(o+4),i++)add(o);}
   return found;
  },{kind,count:gates.length});
  assert.equal(pins.length,gates.length,`${kind} native camera objects`);
  for(const q of pins)assert.ok(q.every((v,i)=>Math.abs(v-(i===3?1:0))<1e-5),`${kind} camera remains upright: ${q}`);
 };
 await upright('minimap');
 assert.equal(await page.locator('.gate-editor').count(),0);assert.ok(!scripts.some(x=>/\/test\/(gate-sync|trap-editor)/.test(x)));
 const gate=gates[0];
 const setup=await page.evaluate(g=>{const s=window.__nfsu2.session,d=new DataView(s.proc.memory.buffer),roads=GateTestModel.readRoads(d.buffer),l=Math.hypot(g.b[0]-g.a[0],g.b[1]-g.a[1]),cx=(g.a[0]+g.b[0])/2,cy=(g.a[1]+g.b[1])/2;const x=cx-(g.b[1]-g.a[1])/l*50,y=cy+(g.b[0]-g.a[0])/l*50;const r=GateTestModel.surfaces(roads,x,y)[0];if(!r)throw Error('No approach road');[r.x,r.y,r.z+1].forEach((v,i)=>d.setFloat32(0x890140+i*4,v,true));d.setUint16(0x8900d0,Math.round((Math.atan2(cy-r.y,cx-r.x)+Math.PI*2)%(Math.PI*2)*65536/(Math.PI*2))&65535,true);d.setUint8(0x8900ce,1);d.setUint8(0x8900cd,1);return {id:g.id,start:[r.x,r.y,r.z+1]};},gate);
 await page.waitForTimeout(2500);await page.keyboard.down('ArrowUp');
 await page.waitForFunction(id=>{const e=document.querySelector('.speedtrap-result');return e&&!e.hidden&&e.dataset.trap===id;},gate.id,{timeout:20000});await page.keyboard.up('ArrowUp');
 const result=await page.locator('.speedtrap-result').innerText();assert.match(result,/GateProd/);assert.match(result,/\d+ (mph|km\/h)/);
 await page.screenshot({path:`${out}/production-pass.png`,animations:'disabled'});
 await page.keyboard.down('ArrowLeft');await page.keyboard.down('ArrowUp');
 for(let i=0;i<12;i++){await page.waitForTimeout(100);await upright('minimap');}
 await page.keyboard.up('ArrowLeft');await page.keyboard.up('ArrowUp');
 await tap('m');await waitLog('[speedtrap] world map camera 0');checkMarkers('world map');await page.waitForTimeout(1500);await upright('world map');await page.screenshot({path:`${out}/production-map.png`});
 await tap('Escape');await page.waitForTimeout(500);await tap('m');await page.waitForTimeout(1500);await upright('world map');
 fs.writeFileSync(`${out}/result.json`,JSON.stringify({gates:gates.length,gate:gate.id,setup,result,dev:false},null,2));
 console.log(`PASS: non-dev game has all ${gates.length} saved markers on both maps; ${gate.id} scores during normal driving; development APIs and UI unavailable.`);
}finally{await page.keyboard.up('ArrowLeft').catch(()=>{});await page.keyboard.up('ArrowUp').catch(()=>{});await page.screenshot({path:`${out}/final.png`}).catch(()=>{});fs.writeFileSync(`${out}/browser.log`,logs.join('\n'));await browser.close();}
