// Repeated real-camera drags across streaming boundaries, without closing the editor or editing gates.
import {chromium} from 'playwright';
import {pickBrowser} from './browser.mjs';
import fs from 'node:fs';
import assert from 'node:assert/strict';
const out=process.argv[2]||'out/web/gate-pan';fs.mkdirSync(out,{recursive:true});
const browser=await chromium.launch({...pickBrowser(),headless:true});
const page=await browser.newPage({viewport:{width:1440,height:1000}}),logs=[];
page.on('console',m=>{logs.push(m.text());fs.appendFileSync(`${out}/live.log`,m.text()+'\n');});
page.on('pageerror',e=>logs.push(`ERROR ${e}`));
const camera=()=>page.evaluate(()=>{const s=window.__nfsu2.session,d=new DataView(s.proc.memory.buffer),st=s.status.read();return {x:d.getFloat32(0x7ec4d0,true),y:d.getFloat32(0x7ec4d4,true),n:st?.get('photo_n'),bits:st?.get('photo')};});
try{
 await page.goto('http://localhost:8081/test/gates.html');await page.locator('#start').click();await page.locator('#play').click();
 for(let i=0;i<300&&!logs.some(x=>x.includes('GATE_EDITOR_READY'));i++)await page.waitForTimeout(500);
 assert.ok(logs.some(x=>x.includes('GATE_EDITOR_READY')),'free roam ready');
 await page.getByRole('button',{name:'Gate editor',exact:true}).click();
 await page.waitForFunction(()=>document.querySelector('.map-help')?.textContent.includes('100 m overhead'),null,{timeout:20000});await page.waitForTimeout(1000);
 const b=await page.locator('.gate-editor-map').boundingBox(),x=b.x+b.width*.65,y=b.y+b.height*.55;
 await page.mouse.move(x,y);for(let i=0;i<3;i++)await page.mouse.wheel(0,250);await page.waitForTimeout(700);
 const first=await camera();
 for(let i=0;i<8;i++){
  const before=await camera(),direction=i<4?1:-1;
  await page.mouse.move(x,y);await page.mouse.down();await page.mouse.move(x+direction*270,y,{steps:16});await page.mouse.up();
  await page.waitForTimeout(2600);
  const after=await camera();assert.ok(Math.hypot(after.x-before.x,after.y-before.y)>10,`pan ${i+1} must move camera: ${JSON.stringify({before,after})}`);
  assert.ok(after.bits&2,`camera reopens after pan ${i+1}`);
  assert.doesNotMatch(await page.locator('.gate-editor-message').innerText(),/Error|Enter free roam/);
 }
 const last=await camera();assert.ok(last.n>=first.n+3,'crossed several district streaming boundaries');
 await page.screenshot({path:`${out}/panned.png`});
 // Zoom out to the city map; travelling here must not touch the streaming anchor.
 await page.mouse.move(x,y);for(let i=0;i<10;i++)await page.mouse.wheel(0,250);
 await page.locator('[data-view="overview"]').waitFor();
 const mapStart=await camera();
 await page.mouse.move(x,y);await page.mouse.down();await page.mouse.move(x-200,y+70,{steps:12});await page.mouse.up();await page.waitForTimeout(700);
 assert.equal((await camera()).n,mapStart.n,'overview travel does not reopen camera');
 assert.ok(await page.locator('.roads path').count()>500);
 await page.screenshot({path:`${out}/overview.png`});
 for(let i=0;i<12;i++)await page.mouse.wheel(0,-250);
 await page.locator('[data-view="loading"]').waitFor({timeout:3000});
 assert.equal(await page.locator('#game').evaluate(e=>getComputedStyle(e).visibility),'hidden');
 await page.screenshot({path:`${out}/overview-loading.png`});
 await page.locator('[data-view="geometry"]').waitFor({timeout:15000});
 await page.screenshot({path:`${out}/new-location.png`});
 // Force another stream; its third-person frames must stay behind the held overhead frame.
 await page.locator('.gate-list button').first().click();
 await page.locator('[data-view="loading"]').waitFor({timeout:3000});
 assert.equal(await page.locator('#game').evaluate(e=>getComputedStyle(e).visibility),'hidden');
 assert.ok(await page.locator('.gate-editor-hold').isVisible());
 const nonblank=await page.locator('.gate-editor-hold').evaluate(c=>{const px=c.getContext('2d').getImageData(0,0,c.width,c.height).data;let n=0;for(let i=0;i<px.length;i+=64)if(px[i]+px[i+1]+px[i+2]>30)n++;return n;});
 assert.ok(nonblank>100,'held overhead frame contains rendered geometry');
 await page.screenshot({path:`${out}/held-frame.png`});
 await page.locator('[data-view="geometry"]').waitFor({timeout:15000});

 assert.ok(!logs.some(x=>x.startsWith('ERROR ')));
 console.log(`PASS: eight consecutive pans; ${last.n-first.n} camera reopenings; editor stayed open and responsive; city navigation and held loading frames verified.`);
}finally{await page.screenshot({path:`${out}/final.png`}).catch(()=>{});fs.writeFileSync(`${out}/browser.log`,logs.join('\n'));await browser.close();}
