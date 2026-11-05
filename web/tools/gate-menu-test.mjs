import { chromium } from 'playwright';
import { pickBrowser } from './browser.mjs';
import assert from 'node:assert/strict';
import fs from 'node:fs';
const out=process.argv[2]||'out/web/gate-menu';fs.mkdirSync(out,{recursive:true});
const browser=await chromium.launch({...pickBrowser(),headless:true});
try{
 for(const dev of [true,false]){
  const context=await browser.newContext({viewport:{width:1280,height:900}}),page=await context.newPage();
  if(!dev)await page.route('**/test/dev.json',route=>route.fulfill({status:404,body:''}));
  await page.goto('http://localhost:8081/');await page.locator('#play').click();
  await page.getByRole('button',{name:'Not now',exact:true}).click();
  await page.waitForFunction(()=>document.body.dataset.phase==='playing',null,{timeout:60000}).catch(async e=>{await page.screenshot({path:`${out}/failure.png`});console.error(await page.locator('body').innerText());throw e;});
  assert.equal(await page.locator('.gate-editor').count(),0,'editor is loaded only on demand');
  await page.keyboard.press('F9');await page.locator('#menu').waitFor({state:'visible'});
  assert.equal(await page.locator('#menu-dev').isVisible(),dev);
  if(dev){
   await page.screenshot({path:`${out}/f9-menu.png`});
   await page.getByRole('button',{name:'Gate editor',exact:true}).click();
   await page.locator('.gate-editor').waitFor({state:'visible'});
   assert.match(await page.locator('.gate-editor header').innerText(),/Live world camera/);
   assert.equal(await page.locator('.gate-editor-dock').isVisible(),false);
   await page.getByRole('button',{name:'Drive',exact:true}).click();await page.keyboard.press('F9');
   await page.locator('#menu [data-act="gate-editor"]').click();
   assert.equal(await page.locator('.gate-editor').count(),1,'reopens existing editor');
  }
  await context.close();
 }
 console.log('PASS: ordinary game URL → F9 → Development → Gate editor; hidden without dev mode; lazy loading and reopening.');
}finally{await browser.close();}
