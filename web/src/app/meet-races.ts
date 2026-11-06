// Server-driven free-roam races. Presentation only: no native race lifecycle or GPS.
import type { GameSession } from './game';
import type { PadSnapshot } from '../input/gamepad';
interface Row { n:string; s:number; p:number; t:number; cash:number; me:number }
interface State {map?:number;meetRace:number;split:number;device:number;phase:number;mine:number;error:number;id:string;go:number;left:number;elapsed:number;distance?:number;rows:Row[];shared:number;saved:number}
const time=(ms:number)=>`${Math.floor(Math.max(0,ms)/60000)}:${String(Math.floor(Math.max(0,ms)/1000)%60).padStart(2,'0')}`;
export class MeetRaces {
  private slots = [this.slot(),this.slot()];
  private timer=window.setInterval(()=>this.draw(),100);
  constructor(private session:()=>GameSession|null,private stage:HTMLElement){
    for(const s of this.slots)stage.append(s.panel,s.count);
    window.addEventListener('keydown',e=>{
      if(e.repeat||e.ctrlKey||e.metaKey||e.altKey||/^(INPUT|TEXTAREA|SELECT)$/.test((e.target as HTMLElement)?.tagName??''))return;
      const seat=e.shiftKey?1:0,s=this.slots[seat];if(!s.state?.phase||performance.now()-s.at>5000)return;
      if(e.code==='KeyJ'&&(s.state.mine===1||s.state.mine===2||(s.state.phase===1&&s.state.mine===0))){e.preventDefault();e.stopImmediatePropagation();this.send(seat,s.state.mine===1||s.state.mine===2?2:1);}
      if(e.code==='KeyK'){e.preventDefault();e.stopImmediatePropagation();this.dismiss(seat);}
    },true);
  }
  private slot(){const panel=document.createElement('aside'),count=document.createElement('div');panel.className='meet-race-panel';count.className='qc-count meet-race-count';panel.hidden=count.hidden=true;return {panel,count,state:null as State|null,at:0,dismissed:'',step:'',resultsUntil:0,rendered:''};}
  show(json:string){
    let state:State;try{state=JSON.parse(json);}catch{return;}
    if(state.meetRace!==1&&state.meetRace!==2)return;
    state.rows = (state.rows as unknown as [string,number,number,number,number,number][]).map(([n,s,p,t,cash,me])=>({n,s,p,t,cash,me}));
    const s=this.slots[state.meetRace-1];if(state.id!==s.state?.id){s.step='';s.resultsUntil=0;}
    if(state.phase>=5&&!s.resultsUntil)s.resultsUntil=performance.now()+5000;
    s.state=state;s.at=performance.now();this.draw();
  }
  /** LB+RB held, A joins and B leaves, independently for each controller. The
   * combination consumes input only while issuing the command. */
  pad(p:PadSnapshot,pressed:number,seat=0):boolean {
    if((p.buttons&(1<<4|1<<5))!==(1<<4|1<<5))return false;

    if(!this.slots[seat].state?.phase||performance.now()-this.slots[seat].at>5000)return false;
    if(pressed&1){this.send(seat,1);return true;}
    if(pressed&2){this.send(seat,2);return true;}return false;
  }
  localPad(p:PadSnapshot,pressed:number,device:number):boolean {
    const r=this.slots[1].state;return !!r?.split && r.device===device && this.pad(p,pressed,1);
  }
  private dismiss(seat:number){const s=this.slots[seat],r=s.state;if(!r||r.mine===1||r.mine===2)return;s.dismissed=r.id;if(r.mine)this.send(seat,2);this.draw();}
  private send(seat:number,action:number){const s=this.slots[seat];if(action===1)s.dismissed='';this.session()?.net?.meetRace?.(seat,action);}
  private button(s:ReturnType<MeetRaces['slot']>,label:string,fn:()=>void){const b=document.createElement('button');b.textContent=label;b.onclick=fn;s.panel.append(b);}
  private draw(){
    const session=this.session(),rect=session?.viewRect();
    for(let seat=0;seat<2;seat++){
      const s=this.slots[seat],r=s.state,age=performance.now()-s.at;
      if(!r?.phase||!session||session.ended||age>5000||!rect){s.panel.hidden=s.count.hidden=true;continue;}
      const active=r.mine===1||r.mine===2,split=!!r.split,h=rect.h/(split?2:1),y=rect.y+(split?seat*h:0);
      s.panel.style.left=`${rect.x+rect.w-285}px`;s.panel.style.top=`${y+h*.17}px`;
      s.count.style.setProperty('--u',`${h/480}px`);
      s.panel.style.maxHeight=`${h*.68}px`;s.count.style.left=`${rect.x+rect.w/2}px`;s.count.style.top=`${y+h*.24}px`;
      const go=r.go-age,elapsed=r.elapsed+age,left=r.left-age;
      const count=active&&r.phase===2?String(Math.max(1,Math.ceil(go/1000))):active&&(r.phase===3||r.phase===4)&&elapsed>=0&&elapsed<1200?'GO!':'';
      s.count.hidden=!count;if(count&&count!==s.step){const n=document.createElement('span');n.textContent=count;s.count.replaceChildren(n);}s.step=count;
      s.panel.hidden=!!r.map||(s.dismissed===r.id&&!active)||(s.resultsUntil>0&&performance.now()>=s.resultsUntil);if(s.panel.hidden)continue;
      const rendered=JSON.stringify([r.phase,r.mine,r.error,r.id,r.rows,r.saved,r.shared,split,Math.floor(go/1000),Math.floor(left/1000),Math.floor(elapsed/1000),r.distance]);
      if(s.rendered===rendered)continue;s.rendered=rendered;
      const focus=(document.activeElement?.parentElement===s.panel)?document.activeElement?.textContent:null;
      s.panel.replaceChildren();const title=document.createElement('strong');title.textContent=`${split?`P${seat+1} · `:''}Meet race`;s.panel.append(title);
      const line=document.createElement('p');
      if(r.phase===1)line.textContent=`Next race · ${time(go)} · ${r.rows.filter(v=>v.s===1).length}/16 joined`;
      else if(r.phase===2)line.textContent='Stay near the meet — get ready!';
      else if(r.phase===3||r.phase===4)line.textContent=`${time(elapsed)} · ${r.distance===undefined?'':`${r.distance} m · `}${r.rows.filter(v=>v.s===3).length} finished`;
      else line.textContent=r.phase===6?'Race cancelled':'Race complete';s.panel.append(line);
      if(r.phase===4){const clock=document.createElement('p');clock.className='meet-race-hurry';clock.textContent=`FINISH · ${time(left)}`;s.panel.append(clock);}
      if(r.error){const error=document.createElement('p');error.textContent='Unable to join this race.';s.panel.append(error);}
      if(r.mine>=4){const status=document.createElement('p');status.textContent=r.mine===4?'You left this race.':r.mine===5?'You did not finish.':'You were disqualified from this race.';s.panel.append(status);}
      if(r.mine===3){const me=r.rows.find(v=>v.me);const prize=document.createElement('p');prize.textContent=`Finished ${me?.p??''}${me?.cash?` · +${me.cash} credits`:''}`;s.panel.append(prize);}
      if(r.phase===1&&r.mine===0)this.button(s,'Join · J',()=>this.send(seat,1));
      if(active)this.button(s,'Leave race · J',()=>this.send(seat,2));
      if(!active)this.button(s,'Dismiss · K',()=>this.dismiss(seat));
      if(r.phase===1&&r.mine===0){const hint=document.createElement('small');hint.textContent='Join at the meet before the countdown starts.';s.panel.append(hint);}
      if(r.mine===1){const hint=document.createElement('small');hint.textContent='Stay within the meet area until GO. Choose your own route.';s.panel.append(hint);}
      if(r.shared){const shared=document.createElement('small');shared.textContent='Prizes go to the shared career bank.';s.panel.append(shared);}
      if(r.mine===3&&r.rows.some(v=>v.me&&v.cash)&&r.saved!==1&&r.saved!==2){const save=document.createElement('small');save.textContent=r.saved<0?'Prize pending: unable to update career cash.':'Prize processing…';s.panel.append(save);}
      if(r.phase>=3){const table=document.createElement('table');
        const rows=[...r.rows].sort((a,b)=>(a.p||99)-(b.p||99));
        for(const row of rows){const tr=document.createElement('tr');for(const txt of [row.p?String(row.p):'—',row.n,row.s===3?`${time(row.t)}.${String(Math.floor(row.t/10)%100).padStart(2,'0')}`:['','Joined','Racing','Finished','Left','DNF','Disqualified'][row.s]??'']){const td=document.createElement('td');td.textContent=txt;tr.append(td);}if(row.me)tr.className='me';table.append(tr);}s.panel.append(table);}
      if(focus)for(const b of s.panel.querySelectorAll('button'))if(b.textContent===focus)b.focus({preventScroll:true});
    }
  }
}
