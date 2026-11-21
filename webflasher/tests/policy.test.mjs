import test from 'node:test';
import assert from 'node:assert/strict';
import {readFile} from 'node:fs/promises';
import {resolve,sep} from 'node:path';
import {pathToFileURL} from 'node:url';
import {validateCatalog,validatePayload,validateImage,validatePartition,validateSecurity,compatiblePartition,fetchPayload,sha256,boundedBody} from '../src/policy.js';
import {payload,table,image,security} from './fixtures.mjs';

test('valid generated payload passes exact policy',async()=>{
  const p=await payload(); await validatePayload(p.catalog,p.files);
  assert.equal(compatiblePartition(p.files[1],p.files[1]),'supported');
  assert.equal(compatiblePartition(new Uint8Array(3072).fill(255),p.files[1]),'blank');
});
for(const [key,value] of Object.entries({extra:true,schemaVersion:2,board:'CoreS3 SE',chipFamily:'ESP32',
  flashSize:8388608,flashMode:'qio',flashFrequency:'40m',sourceCommit:'A'.repeat(40),
  sourceDirty:'false',firmwareVersion:'0.2',releaseTag:'v1.3',applicationVersion:'1.0.3'})) {
  test(`reject catalog ${key}`,async()=>{
    const p=await payload();p.catalog[key]=value;assert.throws(()=>validateCatalog(p.catalog));
  });
}
for(const value of ['../firmware.bin','https://attacker.test/a','/firmware/a','firmware\\bootloader.bin','firmware/%2e%2e/bootloader.bin']) {
  test(`reject path ${value}`,async()=>{
    const p=await payload();p.catalog.parts[0].path=value;assert.throws(()=>validateCatalog(p.catalog));
  });
}
test('reject wrong ordering, sector crossing, size and offset',async()=>{
  for(const change of [p=>p.parts.reverse(),p=>p.parts[0].size=32769,p=>p.parts[1].size=4096,
    p=>p.parts[2].offset=0x9000,p=>p.parts[2].size=1536001,p=>p.parts[0].size=1.2]) {
    const p=await payload();change(p.catalog);assert.throws(()=>validateCatalog(p.catalog));
  }
});
test('artifact corruption fails SHA before any installation',async()=>{
  const p=await payload();p.files[2][40]^=1;
  await assert.rejects(validatePayload(p.catalog,p.files),/SHA-256/);
});
test('malicious metadata cannot authorize a wrong image/header',async()=>{
  for(const offset of [0,2,3,12,23,40,207]) {
    const p=await payload();p.files[2][offset]^=1;p.catalog.parts[2].sha256=await sha256(p.files[2]);
    await assert.rejects(validatePayload(p.catalog,p.files));
  }
});
test('version descriptor mismatch',async()=>{await assert.rejects(validateImage(await image(true),'9.9.9'));});
test('unknown partitions, extra records and invalid checksum rejected',()=>{
  for(const offset of [0,4,64,96,112,150]) {
    const data=table();data[offset]^=1;assert.throws(()=>validatePartition(data));
  }
  assert.throws(()=>compatiblePartition(new Uint8Array(1),table()));
});
test('locked, encrypted, incomplete and unknown security rejected',()=>{
  validateSecurity(security());
  for(const delta of [{flags:1},{flags:4},{flags:1<<30},{flashCryptCnt:1},{keyPurposes:[1,0,0,0,0,0,0]},
    {apiVersion:1},{chipId:0},{keyPurposes:undefined}]) assert.throws(()=>validateSecurity({...security(),...delta}));
  assert.throws(()=>validateSecurity(null));
});
test('catalog missing fails closed',async()=>{
  await assert.rejects(fetchPayload('https://example.test/Quadband/',async()=>({ok:false,status:404})),/unavailable/);
});
test('relative same-origin downloads under repository prefix',async()=>{
  const p=await payload(),urls=[];
  const result=await fetchPayload('https://example.test/Quadband/',async(url,options)=>{
    urls.push(url.href);assert.equal(options.redirect,'error');
    if(url.pathname.endsWith('catalog.json'))return new Response(JSON.stringify(p.catalog));
    const i=p.catalog.parts.findIndex(part=>url.pathname.endsWith(part.path));
    return new Response(p.files[i]);
  });
  assert.deepEqual(result.catalog,p.catalog);
  assert.equal(urls[0],'https://example.test/Quadband/catalog.json');
  assert(urls.every(url=>url.startsWith('https://example.test/Quadband/')));
});
test('real development payload passes browser policy when provided',async t=>{
  const base=process.env.QB_REAL_PAYLOAD ? pathToFileURL(resolve(process.env.QB_REAL_PAYLOAD)+sep) :
    new URL('../.test-payload/',import.meta.url);
  let catalog;
  try {catalog=JSON.parse(await readFile(new URL('catalog.json',base),'utf8'));}
  catch {t.skip('Optional local real export absent');return;}
  const files=await Promise.all(catalog.parts.map(async p=>new Uint8Array(await readFile(new URL(p.path,base)))));
  await validatePayload(catalog,files);assert.equal(catalog.sourceDirty,true);
});
test('chunked downloads exceeding expected size are cancelled without accumulating the body',async()=>{
  let cancelled=false;
  const response=new Response(new ReadableStream({
    start(controller){controller.enqueue(new Uint8Array(100));},
    cancel(){cancelled=true;},
  }));
  await assert.rejects(boundedBody(response,16,16),/exceeds/);
  assert.equal(cancelled,true);
});
test('short, oversized declared, unstreamed and aborted downloads fail closed',async()=>{
  await assert.rejects(boundedBody(new Response(new Uint8Array(3)),4,4),/size mismatch/);
  await assert.rejects(boundedBody(new Response(new Uint8Array(4),{headers:{'content-length':'999'}}),4,4),/size/);
  await assert.rejects(boundedBody({headers:new Headers()},4,4),/Streaming/);
  const abort=new AbortController();abort.abort();
  await assert.rejects(boundedBody(new Response(new Uint8Array(4)),4,4,abort.signal),/cancelled/);
});
test('catalog payloads are bounded before JSON parsing',async()=>{
  await assert.rejects(fetchPayload('https://example.test/Quadband/',async()=>new Response(' '.repeat(32769))),/exceeds/);
});
