import fs from 'node:fs';
import { test } from 'node:test';
import assert from 'node:assert/strict';
import * as esbuild from 'esbuild';
import { chromium } from 'playwright';
import { pickBrowser } from '../tools/browser.mjs';
const js=(await esbuild.build({entryPoints:[new URL('../src/app/meet-races.ts',import.meta.url).pathname],bundle:true,format:'iife',globalName:'M',write:false})).outputFiles[0].text;
const browser=await chromium.launch({...pickBrowser(),headless:true});test.after(()=>browser.close());
test('independent inputs, synchronized countdown, final results and stale cleanup',async()=>{
 const page=await browser.newPage();await page.clock.install();await page.setContent('<div id="stage"></div>');await page.addStyleTag({content:fs.readFileSync(new URL('../src/app/style.css',import.meta.url),'utf8')});await page.addScriptTag({content:js});
 await page.evaluate(()=>{
  window.commands=[];window.races=new M.MeetRaces(()=>({net:{meetRace:(...args)=>window.commands.push(args)},viewRect:()=>({x:0,y:0,w:1280,h:900})}),document.querySelector('#stage'));
  window.state={meetRace:1,split:1,device:0,phase:1,mine:0,error:0,id:'test',go:30000,left:330000,elapsed:-30000,rows:[],shared:1,saved:0};
  window.races.show(JSON.stringify(window.state));window.races.show(JSON.stringify({...window.state,meetRace:2}));
 });
 await page.locator('.meet-race-panel').first().getByRole('button',{name:'Join · J',exact:true}).click();
 await page.keyboard.press('Shift+J');
 await page.evaluate(()=>{window.races.localPad({buttons:49,index:7},1,0);});
 assert.deepEqual(await page.evaluate(()=>window.commands),[[0,1],[1,1],[1,1]]);
 await page.evaluate(()=>window.races.show(JSON.stringify({...window.state,phase:2,mine:1,go:4500})));
 assert.equal(await page.locator('.meet-race-count').first().innerText(),'5');
 assert.ok(await page.locator('.meet-race-count span').first().evaluate(el=>parseFloat(getComputedStyle(el).fontSize))>=70);
 await page.clock.runFor(1100);assert.equal(await page.locator('.meet-race-count').first().innerText(),'4');
 await page.evaluate(()=>window.races.show(JSON.stringify({...window.state,phase:5,mine:3,rows:[['Other',3,2,22345,200,0],['Me',3,1,21123,300,1]],saved:1})));
 assert.doesNotMatch(await page.locator('.meet-race-panel').first().innerText(),/Final results/i);
 assert.match(await page.locator('.meet-race-panel').first().innerText(),/0:21.12/);
 assert.doesNotMatch(await page.locator('.meet-race-panel').first().innerText(),/Prize received|Save your career/);
 await page.keyboard.press('K');assert.deepEqual(await page.evaluate(()=>window.commands.at(-1)),[0,2]);
 await page.evaluate(()=>window.races.show(JSON.stringify({...window.state,id:'left',phase:1,mine:4})));assert.equal(await page.locator('.meet-race-panel').first().getByRole('button',{name:'Join · J',exact:true}).count(),0);
 await page.clock.runFor(5100);assert.ok(await page.locator('.meet-race-panel').first().isHidden());const n=await page.evaluate(()=>window.commands.length);await page.keyboard.press('J');assert.equal(await page.evaluate(()=>window.commands.length),n);await page.close();
});
test('completed results disappear after five seconds despite refreshed snapshots',async()=>{
 const page=await browser.newPage();await page.clock.install();await page.setContent('<div id="stage"></div>');await page.addScriptTag({content:js});
 await page.evaluate(()=>{
  window.races=new M.MeetRaces(()=>({net:{meetRace:()=>{}},viewRect:()=>({x:0,y:0,w:1280,h:900})}),document.querySelector('#stage'));
  window.result={meetRace:1,split:0,device:0,phase:5,mine:3,error:0,id:'finished',go:0,left:0,elapsed:20000,rows:[['Me',3,1,20000,300,1]],shared:0,saved:2};
  window.races.show(JSON.stringify(window.result));
 });
 assert.ok(await page.locator('table').isVisible());
 assert.equal(await page.locator('table tr').first().locator('td').count(),3);
 assert.doesNotMatch(await page.locator('table').innerText(),/\+300/);
 assert.equal(await page.getByRole('button',{name:/View results|Hide results/}).count(),0);
 for(let i=0;i<4;i++){await page.clock.runFor(1000);await page.evaluate(()=>window.races.show(JSON.stringify(window.result)));}
 assert.ok(await page.locator('.meet-race-panel').first().isVisible());
 await page.clock.runFor(1100);await page.evaluate(()=>window.races.show(JSON.stringify(window.result)));
 assert.ok(await page.locator('.meet-race-panel').first().isHidden());
 await page.evaluate(()=>window.races.show(JSON.stringify({...window.result,id:'next',phase:1,mine:0,rows:[]})));
 assert.ok(await page.locator('.meet-race-panel').first().isVisible());
 assert.ok(await page.getByRole('button',{name:'Join · J',exact:true}).isVisible());
 await page.evaluate(()=>window.races.show(JSON.stringify({...window.result,id:'next',phase:3,mine:2,distance:1234})));
 const text=await page.locator('.meet-race-panel').first().innerText();
 assert.match(text,/1234 m/);assert.doesNotMatch(text,/to finish|Results so far/i);await page.close();
});
test('returning to invitation state keeps Join available for the same race',async()=>{
 const page=await browser.newPage();await page.setContent('<div id="stage"></div>');await page.addScriptTag({content:js});
 await page.evaluate(()=>{
  window.commands=[];window.races=new M.MeetRaces(()=>({net:{meetRace:(...args)=>window.commands.push(args)},viewRect:()=>({x:0,y:0,w:1280,h:900})}),document.querySelector('#stage'));
  window.state={meetRace:1,split:0,device:0,phase:1,mine:1,error:0,id:'same-race',go:30000,left:330000,elapsed:-30000,rows:[],shared:0,saved:0};
  window.races.show(JSON.stringify(window.state));
 });
 assert.ok(await page.getByRole('button',{name:'Leave race · J',exact:true}).isVisible());
 await page.evaluate(()=>window.races.show(JSON.stringify({...window.state,mine:0})));
 assert.ok(await page.getByRole('button',{name:'Join · J',exact:true}).isVisible());
 await page.getByRole('button',{name:'Join · J',exact:true}).click();
 assert.deepEqual(await page.evaluate(()=>window.commands),[[0,1]]);await page.close();
});
