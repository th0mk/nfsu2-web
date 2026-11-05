// Development-only live world editor. The server owns the repository catalog.
import type { GameSession } from '../app/game';
import { catalog, length, parseCatalog, readRoads, surfaces, type Gate, type Point, type Road } from './traps/model';
import { viewMatrix, project, unproject } from './traps/projection';
import './traps/editor.css';
import { CatalogSave, CatalogConflict } from './traps/save';
const API='/test/gates/catalog',NS='http://www.w3.org/2000/svg';
const make=<K extends keyof HTMLElementTagNameMap>(tag:K,text='',cls='')=>{const e=document.createElement(tag);e.textContent=text;e.className=cls;return e;};
const svg=(tag:string,attrs:Record<string,string|number>={})=>{const e=document.createElementNS(NS,tag);for(const [k,v] of Object.entries(attrs))e.setAttribute(k,String(v));return e;};
export function attach(session:GameSession,options:{isolated?:boolean}={isolated:true}) {
  const link=make('link');link.rel='stylesheet';link.href='/test/trap-editor.css';document.head.append(link);
  const dock=make('div','','gate-editor-dock'),open=make('button','Gate editor');dock.dataset.nfsu2Ui='';dock.hidden=!options.isolated;dock.append(open);document.body.append(dock);
  const dialog=make('dialog','','gate-editor');dialog.dataset.nfsu2Ui='';dialog.setAttribute('aria-label','Speed trap gate editor');
  const header=make('header'),title=make('div'),close=make('button','Drive');title.append(make('h2','Speed trap gates'),make('p','Live world camera • drag to pan • scroll to zoom'));
  header.append(title,close);
  const toolbar=make('nav'),body=make('div','','gate-editor-body'),mapWrap=make('div','','gate-editor-map');
  const hold=make('canvas','','gate-editor-hold');hold.hidden=true;
  const map=svg('svg',{'aria-label':'Live game world',role:'img',tabindex:0}),roadMarks=svg('g',{class:'roads'}),marks=svg('g');map.append(roadMarks,marks);mapWrap.append(hold,map);
  const help=make('p','','map-help');mapWrap.append(help);
  const aside=make('aside'),list=make('div','','gate-list'),form=make('div'),message=make('p','','gate-editor-message');message.setAttribute('role','status');list.setAttribute('aria-label','Gates');
  aside.append(make('h3','Gates'),list,form,message);body.append(mapWrap,aside);dialog.append(header,toolbar,body);document.body.append(dialog);
  let gates:Gate[]=[],selected=0,roads:Road[]=[],adding:Point|null|undefined,center:Point=[1727,-776,23.6],altitude=100;
  let writer:CatalogSave|null=null;
  let saved='',dirty=false,saving=false,conflict=false,loaded=false,disposed=false,serial=0,saveTimer:ReturnType<typeof setTimeout>|undefined;
  let matrix:number[]|null=null,canvas:HTMLCanvasElement|null=null,home:Node|null=null,next:Node|null=null,frame=0,cameraBusy=false,cameraReady=false;
  let overview=false,holding=false;
  let lastStream:Point|null=null,streamTimer:ReturnType<typeof setTimeout>|undefined;
  const say=(s:string)=>{message.textContent=s;};
  const button=(s:string,fn:()=>void)=>{const b=make('button',s);b.type='button';b.onclick=fn;return b;};
  const current=()=>gates[selected];
  const delay=(ms:number)=>new Promise<void>(r=>setTimeout(r,ms));
  const photo=(kind:number,a=0,b=0)=>session.input?.photo?.(kind,a,b);
  const bits=()=>session.status?.read()?.get('photo')??0;
  async function wait(test:()=>boolean,token:number){const end=performance.now()+10000;while(!test()){if(disposed||!dialog.open||serial!==token)throw Error('Camera closed');if(performance.now()>end)throw Error('Enter free roam to use the live camera.');await delay(50);}}
  function player(){try{const d=new DataView(session.proc.memory.buffer),p=d.getUint32(0xd00000,true),car=p?d.getUint32(p+4,true):0;if(!car)return null;const pos=[0,4,8].map(o=>d.getFloat32(car+0x60+o,true)) as Point;return pos.every(Number.isFinite)?pos:null;}catch{return null;}}
  function road(p:Point){return surfaces(roads,p[0],p[1])[0];}
  function scale(){return dimensions().h/(2*altitude*Math.tan(Math.PI/6));}
  function mapPoint(x:number,y:number):Point{const {w,h}=dimensions(),k=scale();return [center[0]+(x-w/2)/k,center[1]-(y-h/2)/k,center[2]];}
  function visibility(){
    if(canvas)canvas.style.visibility=overview||holding?'hidden':'';
    hold.hidden=overview||!holding||!hold.width;
    roadMarks.setAttribute('display',overview||(holding&&!hold.width)?'':'none');
    mapWrap.dataset.view=overview?'overview':holding?'loading':'geometry';
    help.textContent=overview?'City map • drag to travel • scroll in to load road geometry':holding?'Loading district…':`${Math.round(altitude)} m overhead • scroll out for city map • gate detection: road to +45 m`;
  }
  function readRoadMap(){
    roads=readRoads(session.proc.memory.buffer);
    roadMarks.replaceChildren(...roads.map(r=>svg('path',{d:r.points.map((p,i)=>`${i?'L':'M'}${p[0]},${p[1]}`).join(' ')})));
  }
  function sendCamera(){if(!overview){photo(31,Math.round(center[0]*100),Math.round(center[1]*100));photo(32,Math.round(Math.min(250,altitude)*100),Math.round(center[2]*100));}visibility();}
  async function holdView(){
    if(!holding){
      hold.width=0;
      // WebGPU discards the presented canvas buffer. Read the renderer's next frame,
      // before closing photo mode, instead of copying an already-cleared canvas.
      if(canvas&&cameraReady&&!overview&&session.renderer?.dump){
        const blob=await session.renderer.dump('gate-loading',false);
        if(blob.size){const image=await createImageBitmap(blob);hold.width=image.width;hold.height=image.height;hold.getContext('2d')?.drawImage(image,0,0);image.close();}
      }
    }
    if(disposed||!dialog.open)return;
    holding=true;visibility();
  }
  async function camera(stream=false){
    if(cameraBusy||!dialog.open||overview)return;cameraBusy=true;const token=serial;
    try{
      await holdView();if(disposed||!dialog.open||token!==serial)return;cameraReady=false;
      if(!roads.length)readRoadMap();
      // CAN OPEN (bit 1) is cleared while photo mode is already ON (bit 2).
      // Accept either state before closing the camera to move the streaming anchor.
      await wait(()=>!!(bits()&3),token);
      // Moving the streaming anchor is automatic when panning beyond the loaded neighbourhood.
      if(stream){const target=[...center] as Point,r=road(target);if(r){photo(1,0);await wait(()=>!(bits()&2),token);const d=new DataView(session.proc.memory.buffer);[r.x,r.y,r.z+1].forEach((v,i)=>d.setFloat32(0x890140+i*4,v,true));d.setUint8(0x8900ce,1);d.setUint8(0x8900cd,1);await delay(1200);await wait(()=>!!(bits()&1),token);lastStream=target;}}
      if(disposed||!dialog.open||token!==serial)return;
      photo(16,1);photo(1,1);await wait(()=>!!(bits()&2),token);
      if(overview)return;
      sendCamera();
      await wait(()=>{if(overview)return true;const d=new DataView(session.proc.memory.buffer);return Math.abs(d.getFloat32(0x7ec4d8,true)-d.getFloat32(0x8286a8,true)-Math.min(250,altitude))<.2;},token);
      // Native camera state is published before the renderer presents that frame.
      for(let i=0;i<4;i++)await new Promise<void>(r=>requestAnimationFrame(()=>r()));
      if(disposed||!dialog.open||token!==serial||overview)return;
      cameraReady=true;holding=false;visibility();
    }catch(e){if(dialog.open&&token===serial){cameraReady=!!(bits()&2);overview=true;altitude=Math.max(300,altitude);holding=false;visibility();say(String(e));}}finally{cameraBusy=false;}
  }
  function focus(p:Point){const r=road(p);center=[p[0],p[1],r?.z??p[2]-1];if(overview||lastStream&&Math.hypot(center[0]-lastStream[0],center[1]-lastStream[1])<=100)sendCamera();scheduleStream();}
  function scheduleStream(){clearTimeout(streamTimer);if(overview)return;streamTimer=setTimeout(()=>{if(cameraBusy){scheduleStream();return;}if(!cameraReady||!lastStream||Math.hypot(center[0]-lastStream[0],center[1]-lastStream[1])>100)void camera(true);},250);}
  async function load(){
    const r=await fetch(API,{cache:'no-store'});if(!r.ok)throw Error(`Catalog unavailable (${r.status}). Restart the updated dev server.`);
    const text=await r.text();gates=parseCatalog(text);writer=new CatalogSave(API,r.headers.get('etag')??'',text);saved=catalog(gates);loaded=true;dirty=false;conflict=false;selected=Math.min(selected,Math.max(0,gates.length-1));render();say('Saved • edits apply immediately');
  }
  async function flush(){
    clearTimeout(saveTimer);if(saving||!dirty||conflict||!loaded)return;saving=true;
    try{while(dirty&&!conflict){const text=catalog(gates);say('Saving…');await writer!.write(text);
      saved=text;window.dispatchEvent(new Event('gate-catalog-saved'));dirty=catalog(gates)!==saved;say(dirty?'Saving…':'Saved • ready to drive');
    }}catch(e){if(e instanceof CatalogConflict)conflict=true;say(String(e));}finally{saving=false;}
  }

  function commit(){try{catalog(gates);dirty=catalog(gates)!==saved||!!writer?.pending;clearTimeout(saveTimer);saveTimer=setTimeout(()=>void flush(),200);say('Saving…');render();}catch(e){say(String(e));}}
  function dimensions(){return {w:canvas?.width||1280,h:canvas?.height||720};}
  function point(e:PointerEvent):Point{if(!matrix&&!overview)throw Error('Camera is not ready');const v=(map as SVGSVGElement).createSVGPoint();v.x=e.clientX;v.y=e.clientY;const c=v.matrixTransform((map as SVGSVGElement).getScreenCTM()!.inverse()),{w,h}=dimensions();if(overview)return mapPoint(c.x,c.y);let p=unproject(matrix!,c.x,c.y,center[2],w,h);for(let i=0;i<2;i++){const r=road(p);p=unproject(matrix!,c.x,c.y,r?.z??center[2],w,h);}p[2]=(road(p)?.z??center[2])+1;return p;}
  function draw(){
    if((!matrix&&!overview)||(holding&&!overview&&hold.width>0))return;const {w,h}=dimensions();map.setAttribute('viewBox',`0 0 ${w} ${h}`);marks.replaceChildren();
    roadMarks.setAttribute('transform',`translate(${w/2},${h/2}) scale(${scale()},${-scale()}) translate(${-center[0]},${-center[1]})`);
    const display=(p:Point):[number,number]=>overview||(holding&&!hold.width)?[w/2+(p[0]-center[0])*scale(),h/2-(p[1]-center[1])*scale()]:project(matrix!,[p[0],p[1],p[2]-1],w,h);
    for(let i=0;i<gates.length;i++){const g=gates[i],a=display(g.a),b=display(g.b);if(![...a,...b].every(Number.isFinite))continue;
      marks.append(svg('line',{x1:a[0],y1:a[1],x2:b[0],y2:b[1],class:i===selected?'gate selected':'gate','data-gate':i}));
      if(overview){const x=(a[0]+b[0])/2,y=(a[1]+b[1])/2;marks.append(svg('circle',{cx:x,cy:y,r:6,class:i===selected?'map-pin selected':'map-pin','data-gate':i}));}
      if(i===selected&&!overview)for(const end of ['a','b'] as const){const p=display(g[end]);marks.append(svg('circle',{cx:p[0],cy:p[1],r:9,class:'endpoint','data-end':end}));}
    }
    if(adding){const p=display(adding);marks.append(svg('circle',{cx:p[0],cy:p[1],r:8,class:'pending'}));}
  }
  function render(){
    list.replaceChildren(...gates.map((g,i)=>{const b=button(g.id,()=>{selected=i;adding=undefined;render();focus(g.a.map((v,k)=>(v+g.b[k])/2) as Point);});b.setAttribute('aria-pressed',String(i===selected));return b;}));
    if(!gates.length)list.append(make('p','No gates yet. Add one across a road.'));
    form.replaceChildren();const g=current();if(!g){draw();return;}
    const label=make('label','Gate ID'),id=make('input');id.value=g.id;id.maxLength=32;id.setAttribute('aria-label','Gate ID');label.append(id);form.append(label);
    id.onchange=()=>{const prev=g.id;g.id=id.value.trim();try{catalog(gates);commit();}catch(e){g.id=prev;id.value=prev;say(String(e));}};
    form.append(make('p',`${length(g).toFixed(1)} m wide • 45 m high`));
    const actions=make('div','','gate-editor-actions');actions.append(button('Delete gate',()=>{gates.splice(selected,1);selected=Math.max(0,selected-1);commit();}),button('Start on side 1',()=>void spawn(1)),button('Start on side 2',()=>void spawn(-1)));form.append(actions);draw();
  }
  async function drive(){if(conflict){dialog.close();return false;}while(saving)await delay(50);await flush();if(dirty){say('Resolve the save error before driving.');return false;}dialog.close();return true;}
  async function spawn(side:number){const g=current();if(!g)return;const l=length(g),cx=(g.a[0]+g.b[0])/2,cy=(g.a[1]+g.b[1])/2,p:Point=[cx-(g.b[1]-g.a[1])/l*50*side,cy+(g.b[0]-g.a[0])/l*50*side,(g.a[2]+g.b[2])/2];const r=road(p);if(!r||r.distance>50){say('No nearby approach. Drive to the gate manually.');return;}if(!await drive())return;await delay(150);const d=new DataView(session.proc.memory.buffer);[r.x,r.y,r.z+1].forEach((v,i)=>d.setFloat32(0x890140+i*4,v,true));const angle=(Math.atan2(cy-r.y,cx-r.x)+Math.PI*2)%(Math.PI*2);d.setUint16(0x8900d0,Math.round(angle*65536/(Math.PI*2))&65535,true);d.setUint8(0x8900ce,1);d.setUint8(0x8900cd,1);}
  let drag:{start:Point;center:Point;end?:'a'|'b';original?:Point;matrix:number[];overview:boolean;scale:number}|null=null;
  map.addEventListener('pointerdown',event=>{const e=event as PointerEvent;if(e.button!==0||(!cameraReady&&!overview)||!loaded||conflict||(!matrix&&!overview))return;clearTimeout(streamTimer);
    const end=(e.target as Element).getAttribute('data-end') as 'a'|'b'|null,chosen=(e.target as Element).getAttribute('data-gate');
    if(chosen!==null){selected=Number(chosen);render();return;}
    const p=point(e);
    if(adding!==undefined){if(adding===null){adding=p;say('Click the opposite road edge.');}else{let id=`trap_${gates.length+1}`;while(gates.some(g=>g.id===id))id+='_';const g={id,a:adding,b:p};try{catalog([...gates,g]);gates.push(g);selected=gates.length-1;adding=undefined;commit();}catch(err){say(String(err));}}draw();return;}
    drag={start:p,center:[...center],end:end??undefined,original:end?[...current()[end]]:undefined,matrix:matrix?[...matrix]:[],overview,scale:scale()};(map as SVGSVGElement).setPointerCapture(e.pointerId);
  });
  map.addEventListener('pointermove',event=>{if(!drag||(!matrix&&!overview))return;const p=point(event as PointerEvent);if(drag.end){current()[drag.end]=p;draw();}else{const e=event as PointerEvent,v=(map as SVGSVGElement).createSVGPoint();v.x=e.clientX;v.y=e.clientY;const c=v.matrixTransform((map as SVGSVGElement).getScreenCTM()!.inverse()),{w,h}=dimensions(),at=drag.overview?[drag.center[0]+(c.x-w/2)/drag.scale,drag.center[1]-(c.y-h/2)/drag.scale,0]:unproject(drag.matrix,c.x,c.y,drag.start[2]-1,w,h);center[0]=drag.center[0]+drag.start[0]-at[0];center[1]=drag.center[1]+drag.start[1]-at[1];const r=road(center);if(r)center[2]=r.z;sendCamera();}});
  const endDrag=()=>{if(drag?.end){try{catalog(gates);commit();}catch(e){current()[drag.end]=drag.original!;draw();say(String(e));}}else if(drag)scheduleStream();drag=null;};map.addEventListener('pointerup',endDrag);map.addEventListener('pointercancel',endDrag);
  map.addEventListener('wheel',event=>{
    const e=event as WheelEvent;e.preventDefault();if(drag)return;
    const was=overview;
    altitude=Math.max(10,Math.min(6000,altitude*Math.exp(Math.max(-.25,Math.min(.25,e.deltaY*.001)))));
    if(!overview&&altitude>250){overview=true;adding=undefined;clearTimeout(streamTimer);}
    else if(overview&&altitude<=220){overview=false;const r=road(center);if(r){if(r.distance>50){center[0]=r.x;center[1]=r.y;}center[2]=r.z;}holding=true;hold.width=0;cameraReady=false;scheduleStream();}
    if(was&&!overview||!was&&overview)marks.replaceChildren();
    sendCamera();draw();
  },{passive:false});
  toolbar.append(button('New gate',()=>{if(overview){say('Zoom in to road geometry before placing a gate.');return;}if(!loaded||!cameraReady||conflict)return;if(gates.length>=64){say('Maximum 64 gates.');return;}adding=null;say('Click the first road edge, then the opposite edge.');}));
  close.onclick=()=>void drive();
  dialog.addEventListener('cancel',e=>{if(adding!==undefined){e.preventDefault();adding=undefined;draw();}else if(dirty&&!conflict){e.preventDefault();void drive();}});
  function restore(){serial++;cameraReady=false;holding=false;hold.hidden=true;drag=null;clearTimeout(streamTimer);cancelAnimationFrame(frame);photo(1,0);if(canvas){canvas.style.visibility='';if(home)home.insertBefore(canvas,next?.parentNode===home?next:null);}canvas=null;document.body.classList.remove('gate-editing');session.input?.releaseAll?.();}
  dialog.addEventListener('close',()=>{restore();(document.activeElement as HTMLElement)?.blur();});
  open.onclick=async()=>{
    if(dialog.open)return;dialog.showModal();session.input?.releaseAll?.();document.body.classList.add('gate-editing');
    canvas=document.querySelector<HTMLCanvasElement>('#game');if(canvas){home=canvas.parentNode;next=canvas.nextSibling;mapWrap.prepend(canvas);}
    const tick=()=>{if(!dialog.open||disposed)return;try{matrix=viewMatrix(session.proc.memory.buffer);draw();}catch{}frame=requestAnimationFrame(tick);};tick();
    try{if(!dirty||conflict)await load();else say('Unsaved edits retained; retrying automatically.');readRoadMap();const p=player();if(p){center=[p[0],p[1],p[2]-1];lastStream=[...center];}while(cameraBusy)await delay(50);if(dialog.open){visibility();if(!overview)await camera();}}catch(e){say(String(e));}
  };
  const retryTimer=setInterval(()=>{if(dirty&&!conflict)void flush();},2000);
  const before=(e:BeforeUnloadEvent)=>{if(dirty){e.preventDefault();e.returnValue='';}};window.addEventListener('beforeunload',before);
  const dispose=()=>{disposed=true;restore();clearTimeout(saveTimer);clearInterval(retryTimer);window.removeEventListener('beforeunload',before);dialog.remove();dock.remove();link.remove();};
  void session.exited.then(dispose);return Object.assign(dispose,{open:()=>open.click()});
}
