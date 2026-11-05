// Live native-game enrollment, start, destination and exit checks.
// node web/tools/meet-races-test.mjs [http://localhost:8081] [artifact directory] [--teardown]
import {chromium} from 'playwright';
import {pickBrowser} from './browser.mjs';
import {seedSave} from './save-seed.mjs';
import {spawn} from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import assert from 'node:assert/strict';
const base=process.argv[2]||'http://localhost:8081',out=path.resolve(process.argv[3]||'out/web/meet-races');
fs.mkdirSync(out,{recursive:true});
const finish=process.argv.includes('--finish'),split=process.argv.includes('--split'),reload=process.env.NFSU2_EXPECT_BANK;
const teardown=process.argv.includes('--teardown');
const atIndex=process.argv.indexOf('--at'), at=atIndex<0?[1718,-825,24.6]:process.argv[atIndex+1].split(',').map(Number);
assert.ok(at.length===3 && at.every(Number.isFinite));
const room=`meet-races-${Date.now()}`,track=path.join(out,'parked.csv');
fs.writeFileSync(track,`L,0,0,1,${at[0]-8},${at[1]},${at[2]-.6}\nL,1000000000,0,1,${at[0]-8},${at[1]},${at[2]-.6}\n`);
const bots=spawn(process.execPath,['web/tools/roam-crowd.mjs','--base',base,'--n','24','--seconds','700','--meet-racers','2','--room',room,'--track',track,'--links','1','--stagger','0'],{stdio:['ignore','pipe','pipe']});
for(const stream of [bots.stdout,bots.stderr])stream.on('data',d=>fs.appendFileSync(path.join(out,'bots.log'),d));
const browser=await chromium.launch({...pickBrowser(),headless:true}),page=await browser.newPage({viewport:{width:1280,height:900}}),logs=[];
page.on('console',m=>{logs.push(m.text());fs.appendFileSync(path.join(out,'game.log'),m.text()+'\n');});
const evidence=setInterval(()=>page.screenshot({path:path.join(out,'latest.png')}).catch(()=>{}),15000);
page.on('pageerror',e=>{logs.push(`ERROR ${e}`);fs.appendFileSync(path.join(out,'game.log'),`ERROR ${e}\n`);});
const snapshot=()=>page.evaluate(()=>{
 const d=new DataView(window.__nfsu2.session.proc.memory.buffer);
 return {scene:d.getUint32(0x827b8c,true),flow:d.getUint32(0x8654a4,true)};
});
const until=async(fn,ms=30000)=>{const start=Date.now();while(!await fn()){if(logs.some(x=>x.includes('TRAP in thread')))throw Error('Native game trapped; see game.log');if(Date.now()-start>ms)throw Error('Timed out waiting for race state');await page.waitForTimeout(250);}};
const teleport=async(x,y)=>{await page.evaluate(({x,y,z})=>{const d=new DataView(window.__nfsu2.session.proc.memory.buffer);[x,y,z].forEach((v,i)=>d.setFloat32(0x890140+i*4,v,true));d.setUint16(0x8900d0,0,true);d.setUint8(0x8900ce,1);d.setUint8(0x8900cd,1);},{x,y,z:at[2]});};
const key=async k=>{await page.keyboard.down(k);await page.waitForTimeout(150);await page.keyboard.up(k);await page.waitForTimeout(700);};
const saveCareer=async()=>{
 // Isolated profile uses the game's normal Autosave setting; the Save menu
 // still writes through the original profile serializer and file pipeline.
 await page.evaluate(()=>{const d=new DataView(window.__nfsu2.session.proc.memory.buffer);d.setUint8(0x844ad9,1);d.setUint8(0x844ad8,1);});
 await key('Escape');await key('ArrowRight');await key('Enter');await page.waitForTimeout(2500);await key('Enter');
 await until(()=>logs.some(s=>s.includes('Save: over')&&s.includes('the profile written')),20000);
 await key('Escape');
};
try{
 const seed=process.env.NFSU2_SAVE_SEED,save='meet-race-test';
 if(seed)await seedSave(page,base,save,seed);
 const q=new URLSearchParams({save,...(seed?{}:{fresh:''}),script:`/tests/scripts/meet_races${split?'_split':seed?'_resume':''}.txt`,reg:'/tests/web/seed/registry_perf4_native.txt'});
 for(const e of ['NOMOVIES=1','SNDSYNC=0','GPUVS=1',`NFSU2_FPS=60`,'NFSU2_ROAM=1',`NFSU2_ROAM_ROOM=${room}`,'NFSU2_ROAM_NAME=MeetRaceTest','ROAM_PROFILE=MeetRaceTest',`MEET_X=${at[0]}`,`MEET_Y=${at[1]}`,`MEET_Z=${at[2]}`,`MEET_HEADING=${finish?49152:0}`,'NFSU2_ROAM_WEBTEST=vlan','NFSU2_ROAM_MAX_CARS=0','NFSU2_ROAM_COLLIDE=0'])q.append('env',e);
 await page.goto(`${base}/?${q}`);await page.locator('#play').click();
 await until(()=>logs.some(x=>x.includes('MEET_RACES_READY')),210000);
 if(reload!==undefined){
  const bank=()=>page.evaluate(()=>new DataView(window.__nfsu2.session.proc.memory.buffer).getUint32(0x861e74,true));
  assert.equal(await bank(),Number(reload));await page.waitForTimeout(5000);assert.equal(await bank(),Number(reload));console.log('PASS: recovered prize survived a fresh process without duplicate credit');if(process.argv.includes('--save-only')){await saveCareer();console.log('PASS: recovered prizes written through native Save');}
 }else{
 await until(async()=>await page.locator('.meet-race-panel').first().isVisible(),120000);
 const bank=()=>page.evaluate(()=>new DataView(window.__nfsu2.session.proc.memory.buffer).getUint32(0x861e74,true));
 const initialBank=await bank();
 await page.locator('.meet-race-panel').first().getByRole('button',{name:'Join · J',exact:true}).click();
 if(split){await page.keyboard.press('Shift+J');await until(()=>page.locator('.meet-race-panel').nth(1).getByRole('button',{name:'Leave race · J',exact:true}).count());}
 await page.waitForTimeout(1500);console.log('After click:',await page.locator('.meet-race-panel').first().innerText());
 console.log('Network command bridge:',await page.evaluate(()=>typeof window.__nfsu2.session.net.meetRace));
 await until(async()=>await page.locator('.meet-race-panel').first().getByRole('button',{name:'Leave race · J',exact:true}).count()>0);
 await page.screenshot({path:path.join(out,'enrolled.png')});console.log('Enrolled:',await page.locator('.meet-race-panel').first().innerText());
 await until(async()=>await page.locator('.meet-race-count').first().isVisible(),360000);
 await page.screenshot({path:path.join(out,'countdown.png')});
 assert.equal((await snapshot()).scene,0x827b8c,'No native cinematic is started');
 const starter=await page.evaluate(()=>{const d=new DataView(window.__nfsu2.session.proc.memory.buffer),u=a=>d.getUint32(a,true);let found=0;for(let a=u(0x8a4260),n=0;a&&a!==0x8a4260&&n++<1000;a=u(a)){const m=u(a+8);if(m&&u(m+8)===0x3dae5f2c)found++;}return found;});
 fs.writeFileSync(path.join(out,'starter.json'),JSON.stringify({starter}));assert.equal(starter,0,'Countdown does not spawn a starter NPC');
 await until(async()=>await page.locator('.meet-race-panel').first().innerText().then(s=> /\d+ m ·/.test(s)),15000);
 await page.waitForTimeout(300);
 const finishBlip=await page.evaluate(()=>{
  const d=new DataView(window.__nfsu2.session.proc.memory.buffer),u=a=>d.getUint32(a,true),f=a=>d.getFloat32(a,true);
  let hash=0xffffffff;for(const ch of 'MEET_FINISH_HUD_0')hash=(Math.imul(hash,33)+ch.charCodeAt(0))>>>0;
  const icons=u(0x8364d4),car=u(u(0xd00000)+4),view=u(icons+0x14);
  let zoom=f(0x78e098);if(!(zoom>1))zoom=f(view+0xc0);
  for(let n=u(icons),guard=0;n&&n!==icons&&guard++<512;n=u(n)){
   const v=u(n+0x1c),o=v?u(v):0;if(!o||u(o+0x10)!==hash)continue;
   const proxy=(0xd00000+u(n+8)*4)>>>0;
   return {distance:Math.hypot(f(proxy+0x50)-f(car+0x50),f(proxy+0x54)-f(car+0x54)),reach:.55*.3*f(view+0xb4)/zoom,hidden:!!(u(o+0x1c)&1)};
  }
  return null;
 });
 const ringState=()=>page.evaluate(()=>{
  const d=new DataView(window.__nfsu2.session.proc.memory.buffer),u=a=>d.getUint32(a,true),f=a=>d.getFloat32(a,true);
  let hash=0xffffffff;for(const ch of 'MEET_RING_HUD_0')hash=(Math.imul(hash,33)+ch.charCodeAt(0))>>>0;
  const icons=u(0x8364d4);
  for(let n=u(icons),guard=0;n&&n!==icons&&guard++<512;n=u(n)){
   const v=u(n+0x1c),o=v?u(v):0;if(!o||u(o+0x10)!==hash)continue;
   const data=u(o+0x2c),script=u(o+0x54);
   return {o,script,hidden:!!(u(o+0x1c)&1),time:script?f(script+0x10):0,draw:Array.from({length:21},(_,i)=>u(data+i*4))};
  }return null;
 });
 const ringBefore=await ringState();await page.waitForTimeout(250);const ringAfter=await ringState();
 fs.writeFileSync(path.join(out,'finish-ripple.json'),JSON.stringify({ringBefore,ringAfter}));
 assert.ok(ringBefore&&!ringBefore.hidden,'Native destination ripple is visible on minimap');
 assert.notDeepEqual(ringBefore.draw,ringAfter.draw,'Finish ripple animates');
 assert.ok(finishBlip,'Finish flag exists on minimap');
 assert.ok(!finishBlip.hidden,'Finish flag is visible');
 assert.ok(finishBlip.distance<=finishBlip.reach*.86,`Distant flag stays inside minimap rim: ${JSON.stringify(finishBlip)}`);
 fs.writeFileSync(path.join(out,'finish-blip.json'),JSON.stringify(finishBlip));
 await page.screenshot({path:path.join(out,'racing.png')});
 if(!finish){await page.keyboard.down('m');await page.waitForTimeout(150);await page.keyboard.up('m');await page.waitForTimeout(1500);await page.screenshot({path:path.join(out,'map.png')});
 const markerLog=logs.find(s=>s.includes('[meet-race] map marker seat 1'));assert.ok(markerLog,'Native target marker created');const marker=Number(/at (0x[0-9a-f]+)/.exec(markerLog)[1]);
 const markerState=await page.evaluate(m=>{const d=new DataView(window.__nfsu2.session.proc.memory.buffer),u=a=>d.getUint32(a,true),f=a=>d.getFloat32(a,true),o=u(m+12),data=u(o+44);return {m,o,flags:u(o+28),xy:[f(m+16),f(m+20)],screen:[f(m+28),f(m+32)],draw:Array.from({length:16},(_,i)=>f(data+i*4))};},marker);fs.writeFileSync(path.join(out,'map-marker.json'),JSON.stringify(markerState));assert.equal(markerState.flags&1,0,'Target is visible on the map');
 const mapRing=()=>page.evaluate(marker=>{
  const d=new DataView(window.__nfsu2.session.proc.memory.buffer),u=a=>d.getUint32(a,true);
  let hash=0xffffffff;for(const ch of 'MEET_RING_MAP_0')hash=(Math.imul(hash,33)+ch.charCodeAt(0))>>>0;
  let o=u(marker+12);for(let n=0;u(o+8)&&n++<10000;)o=u(o+8);
  for(let n=0;o&&n++<10000;o=u(o+4))if(u(o+0x10)===hash){const data=u(o+0x2c);return {script:u(o+0x54),hidden:!!(u(o+0x1c)&1),draw:Array.from({length:21},(_,i)=>u(data+i*4))};}
  return null;
 },marker);
 const mapRingBefore=await mapRing();await page.waitForTimeout(250);const mapRingAfter=await mapRing();
 fs.writeFileSync(path.join(out,'map-ripple.json'),JSON.stringify({mapRingBefore,mapRingAfter}));
 assert.ok(mapRingBefore&&!mapRingBefore.hidden,'Destination ripple is visible on world map');
 assert.notEqual(mapRingBefore.draw[14],mapRingAfter.draw[14],'World map ripple expands after native marker placement');
 assert.notEqual(mapRingBefore.draw[3],mapRingAfter.draw[3],'World map ripple fades');
assert.ok(logs.some(s=>s.includes('world map open:')),'Native world map must open');await page.keyboard.down('Escape');await page.waitForTimeout(150);await page.keyboard.up('Escape');}
 if(finish){
  console.log('Before drive:',await page.evaluate(()=>{const d=new DataView(window.__nfsu2.session.proc.memory.buffer),u=a=>d.getUint32(a,true),c=u(u(0xd00000)+4);return {car:c,forward:[0,4,8].map(i=>d.getFloat32(c+0x90+i,true))};}));
  await page.keyboard.down('ArrowUp');
  await page.waitForTimeout(2000);console.log('Driving position:',await page.evaluate(()=>{const d=new DataView(window.__nfsu2.session.proc.memory.buffer),u=a=>d.getUint32(a,true),c=u(u(0xd00000)+4);return [0,4,8].map(i=>d.getFloat32(c+0x60+i,true));}));
  await until(async()=> (await bank())>initialBank,30000);await page.keyboard.up('ArrowUp');
  await page.keyboard.down('Space');await until(()=>page.evaluate(()=>{const d=new DataView(window.__nfsu2.session.proc.memory.buffer),u=a=>d.getUint32(a,true),c=u(u(0xd00000)+4);return Math.hypot(...[0,4,8].map(i=>d.getFloat32(c+0x70+i,true)))<0.4;}),20000);await page.keyboard.up('Space');
  const award=(await bank())-initialBank;assert.ok([100,200,300].includes(award));
  await until(()=>page.locator('.meet-race-panel').first().innerText().then(t=>/Final results/i.test(t)),45000);
  await page.screenshot({path:path.join(out,'results.png')});
  await saveCareer();
  await until(()=>page.locator('.meet-race-panel').first().innerText().then(t=>!t.includes('Save your career')),15000);
  console.log('PASS: native cash award and profile save',initialBank,award,await bank());
  fs.writeFileSync(path.join(out,'cash.json'),JSON.stringify({initialBank,award,bank:await bank()}));
 }else{
 await page.locator('.meet-race-panel').first().getByRole('button',{name:'Leave race · J',exact:true}).click();
 await until(async()=>!await page.locator('.meet-race-panel').first().getByRole('button',{name:'Leave race · J',exact:true}).count());
 if(split)assert.ok(await page.locator('.meet-race-panel').nth(1).getByRole('button',{name:'Leave race · J',exact:true}).count());
 }
 assert.ok(!logs.some(x=>/RuntimeError|memory access out of bounds|^ERROR /.test(x)));
 if(teardown){
  await key('Escape');for(let i=0;i<5;i++)await key('ArrowRight');await key('Enter');await key('ArrowLeft');await key('Enter');
  await until(async()=> (await snapshot()).flow===3,45000);console.log('PASS: native world teardown');
 }
 console.log('PASS: native game enrollment, countdown, destination and '+(finish?'finish/save':'leave')+'; starter count '+starter);
 }
}finally{
 await page.evaluate(async()=>{const root=await navigator.storage.getDirectory(),dir=await root.getDirectoryHandle(window.__nfsu2.save),files=[];
  async function walk(d,rel){for await(const [name,h] of d.entries()){const p=rel?rel+'/'+name:name;if(h.kind==='directory')await walk(h,p);else if(name!=='autotest-script.txt'){const b=new Uint8Array(await (await h.getFile()).arrayBuffer());let s='';for(const v of b)s+=String.fromCharCode(v);files.push([p,btoa(s)]);}}}await walk(dir,'');return files;
 }).then(files=>{for(const [rel,b64]of files){const dest=path.join(out,'seed',rel);fs.mkdirSync(path.dirname(dest),{recursive:true});fs.writeFileSync(dest,Buffer.from(b64,'base64'));}}).catch(e=>console.log('Seed export:',e.message));
 clearInterval(evidence);bots.kill('SIGTERM');await page.screenshot({path:path.join(out,'final.png')}).catch(()=>{});await browser.close();}
