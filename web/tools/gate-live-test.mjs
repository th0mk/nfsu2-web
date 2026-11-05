import {chromium} from 'playwright';
import {pickBrowser} from './browser.mjs';
import fs from 'node:fs';
import assert from 'node:assert/strict';
const out=process.argv[2]||'out/web/gate-live';fs.mkdirSync(out,{recursive:true});
const browser=await chromium.launch({...pickBrowser(),headless:true});
const page=await browser.newPage({viewport:{width:1440,height:1000}}),logs=[];
page.on('console',m=>{logs.push(m.text());fs.appendFileSync(`${out}/live.log`,m.text()+'\n');});page.on('pageerror',e=>logs.push(`ERROR ${e}`));
let original,tag;
try{
 await page.goto('http://localhost:8081/test/gates.html');
 const r=await page.request.get('http://localhost:8081/test/gates/catalog');assert.equal(r.status(),200);original=await r.text();tag=r.headers().etag;
 await page.locator('#start').click();await page.locator('#play').click();
 await page.waitForFunction(()=>document.body.dataset.phase==='playing',null,{timeout:90000});
 for(let i=0;i<240&&!logs.some(x=>x.includes('GATE_EDITOR_READY'));i++)await page.waitForTimeout(500);
 assert.ok(logs.some(x=>x.includes('GATE_EDITOR_READY')),'free roam ready');
 await page.getByRole('button',{name:'Gate editor',exact:true}).click();
 await page.waitForFunction(()=>document.querySelector('.map-help')?.textContent.includes('100 m overhead'),null,{timeout:20000});await page.waitForTimeout(1500);
 await page.screenshot({path:`${out}/live.png`});
 assert.equal(await page.locator('.gate-editor-map #game').count(),1,'actual game canvas in editor');
 for(const name of ['Whole city','Undo','View geometry','Open catalog','Import','Save catalog','Export'])assert.equal(await page.getByRole('button',{name,exact:true}).count(),0);
 const map=page.locator('.gate-editor-map'),box=await map.boundingBox();
 await page.mouse.move(box.x+box.width/2,box.y+box.height/2);await page.mouse.wheel(0,-200);await page.waitForTimeout(500);
 assert.ok(!(await page.locator('.map-help').innerText()).startsWith('100 m'));
 await page.mouse.down();await page.mouse.move(box.x+box.width/2+80,box.y+box.height/2+20,{steps:12});await page.mouse.up();await page.waitForTimeout(1000);
 await page.screenshot({path:`${out}/panned.png`});
 await page.locator('.gate-list button').first().click();await page.waitForTimeout(1800);
 const id=page.getByRole('textbox',{name:'Gate ID',exact:true});await id.fill('live_test');
 const saved=page.waitForResponse(r=>r.url().endsWith('/test/gates/catalog')&&r.request().method()==='PUT');await id.press('Tab');const save=await saved;assert.equal(save.status(),200);tag=save.headers().etag;
 assert.match(await (await page.request.get('http://localhost:8081/test/gates/catalog')).text(),/TRAP\(live_test,/);
 await page.getByRole('button',{name:'Start on side 1',exact:true}).click();
 await page.locator('.gate-editor').waitFor({state:'hidden'});await page.locator('#stage #game').waitFor();
 await page.waitForTimeout(2500);await page.keyboard.down('ArrowUp');
 await page.waitForFunction(()=>document.querySelector('.speedtrap-result[data-trap="live_test"]')?.getAttribute('hidden')===null,null,{timeout:20000});
 await page.keyboard.up('ArrowUp');await page.screenshot({path:`${out}/pass.png`});
 assert.ok(!logs.some(x=>x.startsWith('ERROR ')),logs.filter(x=>x.startsWith('ERROR ')).join('\n'));
 console.log('PASS: live canvas, zoom/pan, simplified UI, automatic source save, immediate server scoring after edit.');
}finally{
 await page.keyboard.up('ArrowUp').catch(()=>{});await page.screenshot({path:`${out}/final.png`}).catch(()=>{});fs.writeFileSync(`${out}/browser.log`,logs.join('\n'));
 if(original&&tag){const r=await page.request.put('http://localhost:8081/test/gates/catalog',{headers:{'If-Match':tag,'Content-Type':'text/plain'},data:original});if(r.status()!==200)console.error('Catalog restore refused; preserve concurrent edit:',r.status());}
 await browser.close();
}
