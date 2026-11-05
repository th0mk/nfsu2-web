import fs from 'node:fs';
import { test } from 'node:test';
import assert from 'node:assert/strict';
import * as esbuild from 'esbuild';
import { chromium } from 'playwright';
import { pickBrowser } from '../tools/browser.mjs';
const code=(await esbuild.build({entryPoints:[new URL('../src/app/speedtrap.ts',import.meta.url).pathname],bundle:true,write:false,format:'iife',globalName:'Trap'})).outputFiles[0].text;
const state={v:1,id:'airport',name:'Airport approach',active:true,distance:40,bearing:0,best:0,last:0,passes:0,sequence:0,event:null,session:'one',gate:[1695,-845,24.6,14],rows:[],nearby:[{id:'airport',distance:40,bearing:0},{id:'second',distance:300,bearing:90}]};
test('only a crossing banner, with top three and one shutter per new crossing',async()=>{
 const browser=await chromium.launch({...pickBrowser(),headless:true});
 try {
  const page=await browser.newPage();
  await page.setContent('<main id="stage" class="stage" style="position:relative;width:800px;height:600px"></main>');
  await page.addStyleTag({content:fs.readFileSync(new URL('../src/app/style.css',import.meta.url),'utf8')});await page.addScriptTag({content:code});
  await page.evaluate(()=>{
   window.clicks=0; window.drive=2; window.metric=1;
   window.panel=new Trap.SpeedTrapPanel(document.querySelector('#stage'));
   panel.start({status:{read:()=>new Map([['drive',drive],['speed_kmh',metric]])},audio:{shutter:()=>clicks++}});
  });
  const send=s=>page.evaluate(s=>panel.receive(s),s);
  await send(state);
  assert.equal(await page.locator('.speedtrap-radar,.speedtrap-panel,.speedtrap-pin').count(),0);

  assert.equal(await page.evaluate(()=>clicks),0);
  const pass={...state,sequence:1,passes:1,best:730,last:730,event:{id:'airport',name:'Airport approach',speed:730,best:true},rows:[{name:'Alice',speed:1000,me:false},{name:'Bob',speed:900,me:false},{name:'<img src=x>',speed:730,me:true},{name:'Fourth',speed:700,me:false}]};
  await send(pass);
  assert.equal(await page.locator('.speedtrap-flash').evaluate(el=>getComputedStyle(el).display),'block','real stylesheet must not hide the flash');
  assert.ok(await page.locator('.speedtrap-flash').evaluate(el=>el.getAnimations().length>0));
  await send(state);await send(pass);
  assert.equal(await page.evaluate(()=>clicks),1);
  assert.match(await page.locator('.speedtrap-result').innerText(),/73 km\/h/);
  assert.ok(await page.locator('.speedtrap-result').isVisible());
  assert.equal(await page.locator('.speedtrap-leaders li').count(),3);
  assert.doesNotMatch(await page.locator('.speedtrap-result').innerText(),/Airport approach|Fourth|New best/);
  assert.equal(await page.locator('.speedtrap-leaders img').count(),0,'names are plain text');
  await send({...pass,id:'second',name:'Another road',best:0,last:0,passes:0});
  assert.equal(await page.evaluate(()=>clicks),1,'switching nearest trap does not replay capture');
  await page.emulateMedia({reducedMotion:'reduce'});
  await page.evaluate(()=>metric=0);
  await send({...pass,sequence:2});
  assert.match(await page.locator('.speedtrap-result > strong').innerText(),/^45 mph$/);
  assert.deepEqual(await page.locator('.speedtrap-leaders b').allTextContents(),['62 mph','56 mph','45 mph']);
  assert.equal(await page.evaluate(()=>document.querySelector('.speedtrap-flash').getAnimations().length),0);
  assert.equal(await page.evaluate(()=>clicks),2);
  await page.evaluate(()=>drive=1);await send({...pass,sequence:3});
  assert.equal(await page.evaluate(()=>clicks),2,'menus silence crossing effects');
  assert.equal(await page.locator('.speedtrap-result').isVisible(),false);
  await page.evaluate(()=>drive=2);await send({...pass,sequence:4,session:'new'});
  assert.equal(await page.evaluate(()=>clicks),2,'new session establishes a baseline');
  await send({...state,sequence:4,session:'new',distance:800,nearby:[]});
  assert.equal(await page.locator('.speedtrap-panel').count(),0);
  await page.evaluate(()=>panel.stop());
 } finally {await browser.close();}
});
