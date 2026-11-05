// A real browser drive, with the game's normal physics. Requires a --dev server and game assets.
// node web/tools/speedtrap-test.mjs [http://localhost:8081] [output folder]
import { chromium } from 'playwright';
import { pickBrowser } from './browser.mjs';
import fs from 'node:fs';
import path from 'node:path';
import assert from 'node:assert/strict';
const base=process.argv[2] || 'http://localhost:8081';
const out=path.resolve(process.argv[3] || 'out/web/speedtrap-test');fs.mkdirSync(out,{recursive:true});
const browser=await chromium.launch({...pickBrowser(),headless:true});
const page=await browser.newPage({viewport:{width:1280,height:800}});
const lines=[];let drive;
await page.addInitScript(()=>{
 window.trapShutters=0;window.trapFlashes=[];
 const animate=Element.prototype.animate;
 Element.prototype.animate=function(...args){
   if(this.classList.contains('speedtrap-flash')){const r=this.getBoundingClientRect();window.trapFlashes.push({display:getComputedStyle(this).display,width:r.width,height:r.height});}
   return animate.apply(this,args);
 };
 const start=AudioBufferSourceNode.prototype.start;
 AudioBufferSourceNode.prototype.start=function(...args){
   if(this.buffer && Math.abs(this.buffer.duration-.18)<.001) window.trapShutters++;
   return start.apply(this,args);
 };
});
try {
 const approaching=new Promise(resolve=>page.on('console',m=>{
   lines.push(m.text()); fs.appendFileSync(path.join(out,'live.log'),m.text()+'\n');
   if(m.text().includes('LOG SHOT trap_approach')) resolve();
 }));
 const q=new URLSearchParams({script:'/tests/scripts/speedtrap_drive.txt',save:`speedtrap-${Date.now()}`,fresh:'',reg:'/tests/web/seed/registry_perf4_native.txt'});
 for(const e of [`TRAP_METRIC=${process.env.TRAP_METRIC || '0'}`,`TRAP_X=${process.env.TRAP_X || '1718'}`,`TRAP_Y=${process.env.TRAP_Y || '-825'}`,`TRAP_ANGLE=${process.env.TRAP_ANGLE || '0x3900'}`,'NOMOVIES=1','SNDSYNC=0','GPUVS=1','NFSU2_ROAM=1',`NFSU2_ROAM_ROOM=trap-test-${Date.now()}`,'NFSU2_ROAM_NAME=TrapTest','ROAM_PROFILE=TrapTest','NFSU2_ROAM_WEBTEST=vlan']) q.append('env',e);
 await page.goto(`${base}/?${q}`);
 await page.locator('#play').waitFor({state:'visible'});await page.locator('#play').click();
 await Promise.race([approaching,new Promise((_,reject)=>{drive=setTimeout(()=>reject(Error('No approach marker within 120 seconds')),120000)})]);
 clearTimeout(drive);
 assert.equal(await page.locator('.speedtrap-panel,.speedtrap-radar').count(),0);
 assert.ok(lines.some(x=>x.includes('[speedtrap] minimap camera 0')), 'native minimap marker created');
 await page.keyboard.down('ArrowUp');
 await page.screenshot({path:path.join(out,'approach.png')});
 await page.waitForFunction(()=>{
   const panel=document.querySelector('.speedtrap-result');
   return panel && !panel.hidden && /(km\/h|mph)/.test(panel.textContent);
 },null,{timeout:10000});
 await page.keyboard.up('ArrowUp');
 const board=await page.locator('.speedtrap-result').innerText();
 assert.match(board,/TrapTest/);assert.match(board,process.env.TRAP_METRIC === '1' ? /\d+ km\/h/ : /\d+ mph/);
 assert.doesNotMatch(board,/\d+\.\d/,'speeds omit decimals');
 assert.ok(await page.locator('.speedtrap-result').isVisible(),'crossing result is on screen');
 assert.equal(await page.evaluate(()=>window.trapShutters),1,'one shutter played through the audio context');
 assert.ok(await page.evaluate(()=>window.trapFlashes.some(f=>f.display!=='none'&&f.width>0&&f.height>0)),'flash is rendered over the game');
 await page.evaluate(()=>Promise.all(document.querySelector('.speedtrap-result').getAnimations().map(a=>a.finished)));
 await page.screenshot({path:path.join(out,'scored.png')});
 fs.writeFileSync(path.join(out,'board.txt'),board);console.log('PASS: physical drive scored and appeared on the room leaderboard\n'+board);
 await page.waitForTimeout(4500); // Separate the car from the camera for the map screenshot.
 const mapReady=new Promise(resolve=>page.on('console',m=>{if(m.text().includes('[speedtrap] world map camera 0'))resolve();}));
 await page.keyboard.press('m',{delay:120});
 await Promise.race([mapReady,new Promise((_,reject)=>{drive=setTimeout(()=>reject(Error('Native world map marker missing')),10000);})]);
 clearTimeout(drive);
 await page.waitForTimeout(750);
 await page.screenshot({path:path.join(out,'map.png')});
 await page.keyboard.press('Escape',{delay:120});
 await page.waitForTimeout(500);
 await page.keyboard.press('m',{delay:120});
 await page.waitForTimeout(750);
 assert.equal(lines.filter(x=>x.includes('[speedtrap] world map camera 0')).length,2,'marker recreated once when reopening the map');
 await page.screenshot({path:path.join(out,'map-reopened.png')});
 console.log('PASS: native minimap and world-map markers, including reopening');
} finally {
 clearTimeout(drive);await page.screenshot({path:path.join(out,'final.png')}).catch(()=>{});fs.writeFileSync(path.join(out,'browser.log'),lines.join('\n'));await browser.close();
}
