// Real server/core -> cached meet -> native animated characters, without opening the map.
// node web/tools/meet-spectators-test.mjs [http://localhost:8081] [artifact directory] [--teardown]
import {chromium} from 'playwright';
import {pickBrowser} from './browser.mjs';
import {spawn} from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import assert from 'node:assert/strict';
const base=process.argv[2]||'http://localhost:8081',out=path.resolve(process.argv[3]||'out/web/meet-spectators');
fs.mkdirSync(out,{recursive:true});
const teardown=process.argv.includes('--teardown'), parked=process.argv.includes('--parked-cars');
const atIndex=process.argv.indexOf('--at'), at=atIndex<0?[1718,-825,24.6]:process.argv[atIndex+1].split(',').map(Number);
assert.ok(at.length===3 && at.every(Number.isFinite));
const minimum=atIndex<0?12:6;
const room=`spectators-${Date.now()}`,track=path.join(out,'parked.csv');
fs.writeFileSync(track,`L,0,0,1,${at[0]-8},${at[1]},${at[2]-.6}\nL,1000000000,0,1,${at[0]-8},${at[1]},${at[2]-.6}\n`);
const bots=spawn(process.execPath,['web/tools/roam-crowd.mjs','--base',base,'--n','20','--seconds','600','--room',room,...(parked?['--parked',`${at[0]},${at[1]},${at[2]-.6}`,'--cars','350Z,RX7,SUPRA,SKYLINE,MIATA,CIVIC']:['--track',track]),'--links','1','--stagger','0'],{stdio:['ignore','pipe','pipe']});
for(const stream of [bots.stdout,bots.stderr])stream.on('data',d=>fs.appendFileSync(path.join(out,'bots.log'),d));
const browser=await chromium.launch({...pickBrowser(),headless:true}),page=await browser.newPage({viewport:{width:1280,height:900}}),logs=[];
page.on('console',m=>{logs.push(m.text());fs.appendFileSync(path.join(out,'game.log'),m.text()+'\n');});
page.on('pageerror',e=>logs.push(`ERROR ${e}`));
const snapshot=()=>page.evaluate(()=>{
 const d=new DataView(window.__nfsu2.session.proc.memory.buffer),u=a=>d.getUint32(a,true),f=a=>d.getFloat32(a,true),models=[];
 for(let a=u(0x8a4260),n=0;a&&a!==0x8a4260&&n<1000;a=u(a),n++){
  const model=u(a+8),bone=u(a+0x60),hash=model?u(model+8):0,skin=bone?u(bone+0xe8):0;
  if(skin&&[0x03d34948,0x03d34947,0x21acbf1c,0x0019ec84,0x03338ff0,0x21acbf1d].includes(hash))models.push({hash,p:[f(bone+0x40),f(bone+0x44),f(bone+0x48)],skin:Array.from({length:384},(_,i)=>u(skin+i*4))});
 }
 return {models,scene:u(0x827b8c),flow:u(0x8654a4)};
});
const until=async(fn,ms=30000)=>{const start=Date.now();while(!await fn()){if(logs.some(x=>x.includes('TRAP in thread')))throw Error('Native game trapped; see game.log');if(Date.now()-start>ms)throw Error('Timed out waiting for crowd state');await page.waitForTimeout(250);}};
const teleport=async(x,y)=>{await page.evaluate(({x,y,z})=>{const d=new DataView(window.__nfsu2.session.proc.memory.buffer);[x,y,z].forEach((v,i)=>d.setFloat32(0x890140+i*4,v,true));d.setUint16(0x8900d0,0,true);d.setUint8(0x8900ce,1);d.setUint8(0x8900cd,1);},{x,y,z:at[2]});};
try{
 const q=new URLSearchParams({fresh:'',script:`/tests/scripts/meet_spectators${teardown?'_exit':''}.txt`,reg:'/tests/web/seed/registry_perf4_native.txt'});
 for(const e of ['NOMOVIES=1','SNDSYNC=0','GPUVS=1',`NFSU2_FPS=${teardown?60:30}`,'NFSU2_ROAM=1',`NFSU2_ROAM_ROOM=${room}`,'NFSU2_ROAM_NAME=SpectatorTest','ROAM_PROFILE=SpectatorTest',`MEET_X=${at[0]}`,`MEET_Y=${at[1]}`,`MEET_Z=${at[2]}`,'NFSU2_ROAM_WEBTEST=vlan',`NFSU2_ROAM_MAX_CARS=${parked?24:0}`,'NFSU2_ROAM_COLLIDE=0'])q.append('env',e);
 await page.goto(`${base}/?${q}`);await page.locator('#play').click();
 await until(()=>logs.some(x=>x.includes('SPECTATORS_READY')),210000);
 await until(async()=> (await snapshot()).models.length>=minimum);
 // Spawning is deliberately spread across frames; measure the completed group.
 await page.waitForTimeout(1200);
 const first=await snapshot();assert.ok(first.models.length<=32);assert.equal(first.scene,0x827b8c);assert.equal(first.flow,6);
 assert.ok(!logs.some(x=>x.includes('screen UI_InGame_WorldMap.fng')),'crowd appears without the map');
 await page.screenshot({path:path.join(out,'spectators.png')});await page.waitForTimeout(1500);
 const animated=await snapshot();assert.notDeepEqual(animated.models.map(m=>m.skin),first.models.map(m=>m.skin),'native skeletons animate');
 for(const model of first.models) assert.ok(Math.abs(model.p[2]-(at[2]-.6))<3.5,'spectator stays on the meet road level');
 const positions=first.models.map(({hash,p})=>({hash,p}));
 // Leaving and returning exercises actor destruction, native resource unload and reload.
 await teleport(at[0]-418,at[1]);await until(async()=>!(await snapshot()).models.length);
 await teleport(at[0],at[1]);await until(async()=> (await snapshot()).models.length===first.models.length);
 const returned=await snapshot();assert.deepEqual(returned.models.map(({hash,p})=>({hash,p})),positions,'same server-owned placements after returning');
 if(teardown){
  await until(()=>logs.some(x=>x.includes('SPECTATORS_EXITED')),90000);
  assert.equal((await snapshot()).models.length,0,'world teardown removes our actors');
  await until(()=>logs.some(x=>x.includes('SPECTATORS_BACK')),120000);
  await until(async()=> (await snapshot()).models.length===first.models.length);
  await page.screenshot({path:path.join(out,'returned-from-menu.png')});
 }
 bots.kill('SIGTERM');await until(async()=>!(await snapshot()).models.length,35000);
 await page.screenshot({path:path.join(out,'disbanded.png')});
 assert.ok(!logs.some(x=>/RuntimeError|memory access out of bounds|^ERROR /.test(x)));
 fs.writeFileSync(path.join(out,'result.json'),JSON.stringify({count:first.models.length,positions,animated:true,despawned:true,menuRoundTrip:teardown},null,2));
 console.log(`PASS: ${first.models.length} animated native spectators, no map or cinematic; stable server layout after leaving/returning; cleanup after meet disbands.${teardown?' Main-menu exit and return passed.':''}`);
}finally{bots.kill('SIGTERM');await page.screenshot({path:path.join(out,'final.png')}).catch(()=>{});await browser.close();}
