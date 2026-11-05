export type Point = [number, number, number];
export interface Gate { id: string; a: Point; b: Point }
export interface Road { id: number; points: Point[] }
export interface Surface { road: number; z: number; distance: number; x: number; y: number }
export const length = (g: Gate) => Math.hypot(g.b[0]-g.a[0], g.b[1]-g.a[1]);
export function validate(gates: Gate[]) {
  if (gates.length > 64) throw Error('Keep at most 64 gates.');
  const ids = new Set<string>();
  for (const g of gates) {
    if (!/^[a-z0-9_]{1,32}$/.test(g.id) || ids.has(g.id)) throw Error('IDs must be unique: 1–32 lowercase letters, digits or underscores.');
    ids.add(g.id);
    if (![g.a,g.b].every(p => Array.isArray(p) && p.length === 3 && p.every(v => Number.isFinite(v) && Math.abs(v)<100000))) throw Error('Coordinates must be finite world coordinates.');
    if (length(g)<2 || length(g)>200) throw Error(`${g.id}: gate width must be 2–200 m.`);
  }
}
export function parseCatalog(text: string): Gate[] {
  const gates = text.split(/\r?\n/).map(s=>s.trim()).filter(s=>s && !s.startsWith('//')).map(s=>{
    const match=/^TRAP\(([^()]*)\)$/.exec(s);
    if (!match) throw Error('Expected TRAP(id, ax, ay, az, bx, by, bz).');
    const parts=match[1].split(',').map(s=>s.trim());
    if (parts.length!==7 || parts.slice(1).some(s=>!s || !/^[+-]?(?:\d+\.?\d*|\.\d+)(?:e[+-]?\d+)?$/i.test(s))) throw Error('Each gate needs six numeric coordinates.');
    return {id:parts[0],a:parts.slice(1,4).map(Number) as Point,b:parts.slice(4).map(Number) as Point};
  });
  validate(gates); return gates;
}
export function catalog(gates: Gate[]): string {
  validate(gates);
  return '// Shared server/native catalog: stable id and the two road-edge endpoints.\n' + gates.map(g=>`TRAP(${g.id}, ${[...g.a,...g.b].map(n=>n.toFixed(3)).join(', ')})`).join('\n')+'\n';
}
/** Traffic road centre lines and heights. Same layout as runtime/walls.c; only read after free roam loads. */
export function readRoads(buffer: ArrayBufferLike): Road[] {
  const d=new DataView(buffer), roads: Road[]=[];
  const ok=(p:number,n:number)=>p>=0x400000 && p+n<=d.byteLength && (p+n<=0x28000000 || p>=0x50000000 && p+n<=0x7ffe0000);
  for(let id=0;id<2048;id++) {
    const r=d.getUint32(0x88a334+id*4,true);
    if(!ok(r,0x8c) || d.getInt16(r+0xa,true)!==id) continue;
    const n=d.getInt16(r+0x34,true);
    if(n<2 || n>=4096 || !ok(r+0x8c,n*0x38)) continue;
    const points:Point[]=[];
    for(let i=0;i<n;i++) {const p=r+0x8c+i*0x38;points.push([d.getFloat32(p,true),d.getFloat32(p+4,true),d.getInt16(p+0x12,true)/8]);}
    if(points.every(p=>p.every(v=>Number.isFinite(v)&&Math.abs(v)<100000))) roads.push({id,points});
  }
  return roads;
}
export function surfaces(roads: Road[], x: number, y: number): Surface[] {
  return roads.map(road=>{
    let best:Surface={road:road.id,z:0,distance:Infinity,x,y};
    for(let i=1;i<road.points.length;i++) {
      const a=road.points[i-1],b=road.points[i],dx=b[0]-a[0],dy=b[1]-a[1],l=dx*dx+dy*dy;
      const t=l?Math.max(0,Math.min(1,((x-a[0])*dx+(y-a[1])*dy)/l)):0;
      const px=a[0]+dx*t,py=a[1]+dy*t,distance=Math.hypot(x-px,y-py);
      if(distance<best.distance) best={road:road.id,z:a[2]+(b[2]-a[2])*t,distance,x:px,y:py};
    }
    return best;
  }).sort((a,b)=>a.distance-b.distance).filter((s,i,list)=>i===0 || s.distance<Math.max(35,list[0].distance+15)).slice(0,12);
}
/** Draft geometry test only: no network score or career changes. */
export function crossing(g: Gate, a: Point, b: Point): {heightError:number; direction:number} | null {
  const dx=g.b[0]-g.a[0],dy=g.b[1]-g.a[1],l=dx*dx+dy*dy;
  if(l<4) return null;
  const side=(p:Point)=>dx*(p[1]-g.a[1])-dy*(p[0]-g.a[0]);
  const sa=side(a),sb=side(b);
  if(!((sa<0&&sb>=0)||(sa>0&&sb<=0))) return null;
  const t=sa/(sa-sb),p=a.map((v,i)=>v+(b[i]-v)*t) as Point;
  const u=((p[0]-g.a[0])*dx+(p[1]-g.a[1])*dy)/l;
  if(u<0 || u>1) return null;
  return {heightError:Math.abs(p[2]-(g.a[2]+u*(g.b[2]-g.a[2]))),direction:sa<0?1:2};
}
