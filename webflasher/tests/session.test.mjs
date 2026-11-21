import test from 'node:test';
import assert from 'node:assert/strict';
import {FlashSession,VerifiedHardReset} from '../src/session.js';
import {md5} from '../src/policy.js';
import {payload,security} from './fixtures.mjs';

async function fixture(overrides={},options={}) {
  const p=await payload(),calls=[];
  const port=new EventTarget();
  const loader={
    chip:{CHIP_NAME:'ESP32-S3'},secureDownloadMode:false,IS_STUB:true,
    detectChip:async()=>{calls.push('detect');},
    getSecurityInfo:async()=>security(),runStub:async()=>{calls.push('stub');},
    readFlashId:async()=>0x1840ef,
    readFlash:async(addr,size)=>{assert.equal(addr,0x8000);assert.equal(size,3072);return p.files[1];},
    writeFlash:async opts=>{
      calls.push('write');assert.equal(opts.eraseAll,false);assert.equal(opts.flashMode,'dio');
      assert.equal(opts.flashFreq,'80m');assert.equal(opts.flashSize,'16MB');
      assert.deepEqual(opts.fileArray.map(f=>f.address),[0,32768,65536]);
      opts.fileArray.forEach(({data},i)=>{
        assert(data instanceof Uint8Array);assert.equal(opts.calculateMD5Hash(data),md5(p.files[i]));
        opts.reportProgress(i,data.length,data.length);
      });
    },
    flashMd5sum:async(addr,size)=>{
      calls.push(`md5:${addr}`);const i=p.catalog.parts.findIndex(part=>part.offset===addr);
      assert.equal(size,p.files[i].length);return md5(p.files[i]);
    },
    after:async mode=>{assert.equal(mode,'hard_reset');calls.push('reset');},
    ...overrides,
  };
  const transport={disconnect:async()=>{calls.push('close');}};
  const session=new FlashSession({requestPort:async()=>{calls.push('port');return port;},
    createDriver:()=>({loader,transport}),...options});
  return {p,calls,loader,transport,session,port};
}
test('inspect does not write; explicit install verifies all images before reset and closes',async()=>{
  const {p,calls,session}=await fixture();
  await session.inspect(p,true);assert.equal(session.state,'ready');assert(!calls.includes('write'));
  await session.install(true);
  assert.equal(session.state,'success');
  assert.deepEqual(calls,['port','detect','stub','write','md5:0','md5:32768','md5:65536','reset','close']);
  assert.equal(session.driver,null);
});
test('acknowledgment required before port chooser',async()=>{
  const {p,calls,session}=await fixture();await assert.rejects(session.inspect(p,false));
  assert.deepEqual(calls,[]);
});
test('explicit approval required before write',async()=>{
  const {p,calls,session}=await fixture();await session.inspect(p,true);
  await assert.rejects(session.install(false));assert(!calls.includes('write'));assert(calls.includes('close'));
});
test('permission cancellation never opens driver',async()=>{
  const {p,calls,session}=await fixture({}, {requestPort:async()=>{throw new Error('Permission cancelled');}});
  await assert.rejects(session.inspect(p,true),/Permission cancelled/);assert.deepEqual(calls,[]);
});
for(const [name,overrides] of [
  ['wrong chip',{chip:{CHIP_NAME:'ESP32'}}],
  ['wrong capacity',{readFlashId:async()=>0x1740ef}],
  ['unknown capacity',{readFlashId:async()=>0xffffff}],
  ['locked security',{getSecurityInfo:async()=>({...security(),flags:1})}],
  ['unreadable security',{getSecurityInfo:async()=>{throw new Error('unknown');}}],
  ['no stub',{IS_STUB:false}],
  ['partition mismatch',{readFlash:async()=>new Uint8Array(3072)}],
]) test(`reject ${name} before flash and close`,async()=>{
  const {p,calls,session}=await fixture(overrides);
  await assert.rejects(session.inspect(p,true));assert(!calls.includes('write'));assert(calls.includes('close'));
});
test('blank partition alone is not a fully blank device',async()=>{
  const {p,calls,session}=await fixture({readFlash:async()=>new Uint8Array(3072).fill(255),flashMd5sum:async()=> '0'.repeat(32)});
  await assert.rejects(session.inspect(p,true),/not fully blank/);assert(!calls.includes('write'));
});
test('fully blank device accepted without erase-all',async()=>{
  const blankMd5=md5(new Uint8Array(16777216).fill(255));
  const {p,session}=await fixture({readFlash:async()=>new Uint8Array(3072).fill(255),flashMd5sum:async()=>blankMd5});
  assert.equal(await session.inspect(p,true),'blank');session.cancel();
});
test('write interruption closes without success/reset/retry',async()=>{
  let count=0;
  const {p,calls,session}=await fixture({writeFlash:async()=>{count++;throw new Error('interrupted');}});
  await session.inspect(p,true);await assert.rejects(session.install(true),/interrupted/);
  assert.equal(count,1);assert(!calls.includes('reset'));assert.equal(session.state,'failed');assert(calls.includes('close'));
});
test('hash callback must run for all three images',async()=>{
  const {p,calls,session}=await fixture({writeFlash:async()=>{}});
  await session.inspect(p,true);await assert.rejects(session.install(true),/every programmed image/);
  assert(!calls.includes('reset'));
});
test('programmed byte mismatch refuses reset',async()=>{
  const {p,calls,session}=await fixture({flashMd5sum:async()=> 'f'.repeat(32)});
  await session.inspect(p,true);await assert.rejects(session.install(true),/verification failed/);
  assert(!calls.includes('reset'));assert(calls.includes('close'));
});
test('disconnect during inspection cannot write',async()=>{
  let sessionRef;
  const f=await fixture({readFlashId:async()=>{sessionRef.cancel('USB disconnected');return 0x1840ef;}});
  sessionRef=f.session;await assert.rejects(f.session.inspect(f.p,true),/Cancelled/);
  assert(!f.calls.includes('write'));assert(f.calls.includes('close'));
});
test('cancel holds exclusive writer until outstanding promise settles',async()=>{
  let release;
  const waiting=new Promise(resolve=>{release=resolve;});
  const f=await fixture({writeFlash:async()=>waiting});await f.session.inspect(f.p,true);
  const install=f.session.install(true);await new Promise(resolve=>setTimeout(resolve,10));
  f.session.cancel();assert.equal(f.session.busy,true);
  await assert.rejects(f.session.inspect(f.p,true),/Another operation/);
  release();await assert.rejects(install,/Cancelled/);assert(!f.calls.includes('reset'));
});
test('timeout does not start a concurrent operation or claim success',async()=>{
  let release;
  const waiting=new Promise(resolve=>{release=resolve;});
  const f=await fixture({writeFlash:async()=>waiting},{timeout:30});
  await f.session.inspect(f.p,true);const install=f.session.install(true);
  await new Promise(resolve=>setTimeout(resolve,60));assert.equal(f.session.cancelled,true);assert.equal(f.session.busy,true);
  release();await assert.rejects(install,/Cancelled/);assert(!f.calls.includes('reset'));
});
test('cleanup failure poisons session rather than permitting new writer',async()=>{
  const f=await fixture();f.transport.disconnect=async()=>{throw new Error('locked');};
  await f.session.inspect(f.p,true);await assert.rejects(f.session.install(true),/cleanup failed/);
  await assert.rejects(f.session.inspect(f.p,true),/cleanup/);
});
test('hard reset deasserts DTR before RTS pulse for USB-JTAG',async()=>{
  const calls=[];
  await new VerifiedHardReset({setDTR:async b=>calls.push(['dtr',b]),setRTS:async b=>calls.push(['rts',b]),
    getVid:()=>0x303a,getPid:()=>0x1001}).reset();
  assert.deepEqual(calls,[['dtr',false],['rts',true],['rts',false]]);
});
test('cancel during final serial cleanup cannot be overwritten by success',async()=>{
  let finish,closing;
  const started=new Promise(resolve=>{closing=resolve;});
  const f=await fixture();
  f.transport.disconnect=()=>{closing();return new Promise(resolve=>{finish=resolve;});};
  await f.session.inspect(f.p,true);
  const install=f.session.install(true);
  await started;f.session.cancel();finish();
  await assert.rejects(install,/Cancelled/);
  assert.equal(f.session.state,'failed');
  assert.equal(f.session.busy,false);
});
