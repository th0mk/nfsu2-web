import test from 'node:test';
import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
const repo=fileURLToPath(new URL('../../',import.meta.url));
for(const [source,expected] of [['meet_race_core_unit','meet race core:'],['meet_cash_unit','meet cash:']]){
  test(source,()=>{
    const dir=mkdtempSync(path.join(tmpdir(),'meet-race-'));
    try {
      const exe=path.join(dir,source);
      execFileSync('cc',['-std=c11','-D_DARWIN_C_SOURCE','-D_POSIX_C_SOURCE=200809L','-O2','-Iruntime',`tests/${source}.c`,'-lm','-o',exe],{cwd:repo});
      assert.ok(execFileSync(exe,{encoding:'utf8'}).includes(expected));
    }finally{rmSync(dir,{recursive:true,force:true});}
  });
}
