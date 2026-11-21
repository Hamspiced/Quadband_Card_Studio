import test from 'node:test';
import assert from 'node:assert/strict';
import {SafeTransport} from '../src/transport.js';

test('failed serial writes always release their writable lock and permit port cleanup',async()=>{
  let closed=false;
  const writable=new WritableStream({write(){throw new Error('USB write failed');}});
  const transport=new SafeTransport({writable,readable:null,close:async()=>{closed=true;}});
  await assert.rejects(transport.write(new Uint8Array([1])),/USB write failed/);
  assert.equal(writable.locked,false);
  await transport.disconnect();
  assert.equal(closed,true);
  await assert.rejects(transport.write(new Uint8Array([1])),/closed/);
});

test('safe transport preserves upstream SLIP framing and releases successful writes',async()=>{
  const packets=[];
  const writable=new WritableStream({write(data){packets.push(Array.from(data));}});
  const transport=new SafeTransport({writable});
  await transport.write(new Uint8Array([0xc0,0xdb,1]));
  assert.deepEqual(packets,[[0xc0,0xdb,0xdc,0xdb,0xdd,1,0xc0]]);
  assert.equal(writable.locked,false);
});
