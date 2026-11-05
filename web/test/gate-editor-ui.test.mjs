import fs from 'node:fs';
import {test} from 'node:test';
import assert from 'node:assert/strict';
import {chromium} from 'playwright';
import {pickBrowser} from '../tools/browser.mjs';
import * as esbuild from 'esbuild';
const code=(await esbuild.build({entryPoints:[new URL('../src/test/trap-editor.ts',import.meta.url).pathname],bundle:true,write:false,format:'iife',globalName:'Editor',loader:{'.css':'empty'}})).outputFiles[0].text;
test('live editor auto-saves, handles conflicts, lists/deletes all gates and restores the canvas',async()=>{
 const browser=await chromium.launch({...pickBrowser(),headless:true});
 try{
  const page=await browser.newPage();let disk='TRAP(airport,1711,-773,24.6,1743,-779,24.6)\nTRAP(other,1911,-773,24.6,1943,-779,24.6)\n',version=1,writes=0,failOnce=true;
  await page.route('http://editor.test/**',async route=>{
   if(route.request().url().endsWith('/test/gates/catalog')){
    if(route.request().method()==='PUT'){
     if(failOnce){failOnce=false;return route.fulfill({status:503});}
     if(route.request().headers()['if-match']!==`"${version}"`)return route.fulfill({status:412});
     disk=route.request().postData();version++;writes++;
    }
    return route.fulfill({body:disk,headers:{etag:`"${version}"`}});
   }
   return route.fulfill({contentType:'text/html',body:'<!doctype html><body><div id="stage"><canvas id="game" width="1280" height="720"></canvas></div></body>'});
  });
  await page.goto('http://editor.test/');await page.addScriptTag({content:code});
  await page.addStyleTag({content:fs.readFileSync(new URL('../src/test/traps/editor.css',import.meta.url),'utf8')});
  await page.evaluate(()=>{
   const memory={buffer:new ArrayBuffer(32*1024*1024)},d=new DataView(memory.buffer),r=0x1000000;
   d.setUint32(0x88a334,r,true);d.setInt16(r+0xa,0,true);d.setInt16(r+0x34,2,true);
   for(let i=0;i<2;i++){const p=r+0x8c+i*0x38;d.setFloat32(p,1727,true);d.setFloat32(p+4,-900+i*300,true);d.setInt16(p+0x12,192,true);}
   window.photoBits=1;window.events=[];
   window.session={proc:{memory},status:{read:()=>new Map([['drive',2],['photo',photoBits]])},input:{releaseAll(){},photo(k,a,b){events.push([k,a,b]);if(k===1)photoBits=a?2:1;if(k===32){d.setFloat32(0x7ec4d8,(a+b)/100,true);d.setFloat32(0x8286a8,b/100,true);}}},exited:new Promise(()=>{})};
   window.editor=Editor.attach(session,{isolated:false});editor.open();
  });
  await page.getByRole('status').filter({hasText:'Saved'}).waitFor();
  assert.equal(await page.locator('.gate-editor-map #game').count(),1);
  assert.equal(await page.locator('.gate-editor-dock').isVisible(),false);
  assert.equal(await page.locator('.gate-list button').count(),2);
  // Native bit 1 means CAN OPEN only while closed; an open camera has bit 2, never both.
  for(let i=0;i<2;i++){
   await page.evaluate(i=>{const d=new DataView(session.proc.memory.buffer);d.setUint32(0xd00000,0x1200000,true);d.setUint32(0x1200004,0x1210000,true);d.setFloat32(0x1210060,1727+i*200,true);d.setFloat32(0x1210064,-776,true);d.setFloat32(0x1210068,25,true);events.length=0;},i);
   await page.locator('.gate-list button').nth(i).click();
   await page.waitForFunction(()=>events.some(e=>e[0]===1&&e[1]===0),null,{timeout:2500});
   await page.waitForFunction(()=>events.some(e=>e[0]===1&&e[1]===1)&&photoBits===2,null,{timeout:4000});
  }

  const map=page.locator('.gate-editor-map');
  await page.locator('.gate-editor-map[data-view="geometry"]').waitFor();
  const box=await map.boundingBox();await page.mouse.move(box.x+box.width/2,box.y+box.height/2);
  for(let i=0;i<7;i++)await page.mouse.wheel(0,250);
  await page.locator('.gate-editor-map[data-view="overview"]').waitFor();
  assert.equal(await page.locator('.roads path').count(),1);
  assert.equal(await page.locator('#game').evaluate(e=>e.style.visibility),'hidden');
  await page.evaluate(()=>events.length=0);
  await page.mouse.down();await page.mouse.move(box.x+box.width/2+100,box.y+box.height/2,{steps:8});await page.mouse.up();await page.waitForTimeout(400);
  assert.equal(await page.evaluate(()=>events.some(e=>e[0]===1)),false,'overview panning does not load districts');
  for(let i=0;i<7;i++)await page.mouse.wheel(0,-250);
  await page.locator('.gate-editor-map[data-view="geometry"]').waitFor();
  assert.equal(await page.locator('#game').evaluate(e=>e.style.visibility),'');
  await page.locator('.gate-list button').first().click();
  const id=page.getByRole('textbox',{name:'Gate ID'});
  await id.fill('manual_gate');await id.press('Tab');await page.getByRole('status').filter({hasText:'Saved • ready'}).waitFor();
  assert.equal(writes,1);assert.match(disk,/TRAP\(manual_gate,/);
  version++;disk+='// external edit\n';
  await id.fill('unsaved');await id.press('Tab');await page.getByRole('status').filter({hasText:'changed on disk'}).waitFor();assert.equal(writes,1);
  await page.evaluate(()=>document.querySelector('.gate-editor').close());await page.locator('#stage #game').waitFor();await page.evaluate(()=>editor.open());
  await page.getByRole('status').filter({hasText:'Saved'}).waitFor();assert.equal(await id.inputValue(),'manual_gate');
  await page.getByRole('button',{name:'Delete gate',exact:true}).click();await page.getByRole('status').filter({hasText:'Saved • ready'}).waitFor();
  assert.equal(writes,2);assert.equal(await page.locator('.gate-list button').count(),1);
  await page.getByRole('button',{name:'Delete gate',exact:true}).click();await page.getByRole('status').filter({hasText:'Saved • ready'}).waitFor();
  assert.equal(writes,3);assert.doesNotMatch(disk,/TRAP\(/);assert.equal(await page.locator('.gate-list button').count(),0);
  await page.getByRole('button',{name:'Drive',exact:true}).click();await page.locator('#stage #game').waitFor();
  await page.evaluate(()=>editor());assert.equal(await page.locator('.gate-editor').count(),0);
 }finally{await browser.close();}
});
