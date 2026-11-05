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
try {
 const approaching=new Promise(resolve=>page.on('console',m=>{
   lines.push(m.text()); fs.appendFileSync(path.join(out,'live.log'),m.text()+'\n');
   if(m.text().includes('LOG SHOT survey')) resolve();
 }));
 const q=new URLSearchParams({script:'/tests/scripts/speedtrap_survey.txt',save:`speedtrap-${Date.now()}`,fresh:'',reg:'/tests/web/seed/registry_perf4_native.txt'});
 for(const e of ['NOMOVIES=1','SNDSYNC=0','GPUVS=1','NFSU2_ROAM=1','NFSU2_ROAM_NAME=TrapTest','ROAM_PROFILE=TrapTest','NFSU2_ROAM_WEBTEST=vlan']) q.append('env',e);
 await page.goto(`${base}/?${q}`);
 await page.locator('#play').waitFor({state:'visible'});await page.locator('#play').click();
 await Promise.race([approaching,new Promise((_,reject)=>{drive=setTimeout(()=>reject(Error('No approach marker within 120 seconds')),120000)})]);
 clearTimeout(drive);
 await page.screenshot({path:path.join(out,'survey.png')});
} finally {
 clearTimeout(drive);await page.screenshot({path:path.join(out,'final.png')}).catch(()=>{});fs.writeFileSync(path.join(out,'browser.log'),lines.join('\n'));await browser.close();
}
