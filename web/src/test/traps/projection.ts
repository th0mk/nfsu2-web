import type { Point } from './model';
/** Native eView's row-vector view*projection matrix, the same transform used by world markers. */
export function viewMatrix(buffer:ArrayBufferLike): number[] {
  const d=new DataView(buffer),base=d.getUint32(0x832e50,true)+0x80;
  const m=Array.from({length:16},(_,i)=>d.getFloat32(base+i*4,true));
  if(!m.every(Number.isFinite)||m.every(x=>x===0))throw Error('The world camera is not ready.');
  return m;
}
export function project(m:number[],p:Point,w:number,h:number):[number,number] {
  const [x,y,z]=p,k=x*m[3]+y*m[7]+z*m[11]+m[15];
  return [(1+(x*m[0]+y*m[4]+z*m[8]+m[12])/k)*w/2,(1-(x*m[1]+y*m[5]+z*m[9]+m[13])/k)*h/2];
}
/** Intersect a screen ray with a chosen road-height plane, without flattening raised roads into the ground. */
export function unproject(m:number[],x:number,y:number,z:number,w:number,h:number):Point {
  const u=x/w*2-1,v=1-y/h*2;
  const a=m[0]-u*m[3],b=m[4]-u*m[7],c=-(m[8]-u*m[11])*z-m[12]+u*m[15];
  const d=m[1]-v*m[3],e=m[5]-v*m[7],f=-(m[9]-v*m[11])*z-m[13]+v*m[15],det=a*e-b*d;
  if(Math.abs(det)<1e-9)throw Error('Camera is too shallow to place a gate. Capture an overhead view.');
  return [(c*e-b*f)/det,(a*f-c*d)/det,z];
}
