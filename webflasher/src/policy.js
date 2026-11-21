import CryptoJS from 'crypto-js';

export const specs = [
  ['bootloader', 'firmware/bootloader.bin', 0, 32768],
  ['partition-table', 'firmware/partition-table.bin', 32768, 3072],
  ['application', 'firmware/quadband_cores3.bin', 65536, 1536000],
];
export function insist(value, message) { if (!value) throw new Error(message); }
const exact = (object, keys) => object && typeof object === 'object' &&
  Object.keys(object).sort().join(',') === keys.split(' ').sort().join(',');
const semver = /^[0-9]+\.[0-9]+\.[0-9]+$/;
export const hex = data => Array.from(data, b => b.toString(16).padStart(2, '0')).join('');
export const md5 = data => CryptoJS.MD5(CryptoJS.lib.WordArray.create(data)).toString();
export const sha256 = async data => hex(new Uint8Array(await crypto.subtle.digest('SHA-256', data)));
const text = data => new TextDecoder().decode(data).split('\0')[0];
const equal = (a, b) => a.length === b.length && a.every((v, i) => v === b[i]);

export function validateCatalog(c) {
  insist(exact(c, 'schemaVersion board chipFamily flashSize flashMode flashFrequency applicationVersion firmwareVersion releaseTag sourceCommit sourceDirty parts'), 'Malformed catalog fields');
  insist(c.schemaVersion === 1 && c.board === 'M5Stack CoreS3' && c.chipFamily === 'ESP32-S3' &&
    c.flashSize === 16777216 && c.flashMode === 'dio' && c.flashFrequency === '80m', 'Unsupported firmware target');
  insist(['applicationVersion','firmwareVersion','releaseTag','sourceCommit'].every(key=>typeof c[key] === 'string') &&
    semver.test(c.applicationVersion) && semver.test(c.firmwareVersion) &&
    /^v(?:[0-9]+\.[0-9]+\.[0-9]+(?:-[0-9A-Za-z]+(?:[.-][0-9A-Za-z]+)*)?|1\.0[12])$/.test(c.releaseTag) &&
    /^[0-9a-f]{40}$/.test(c.sourceCommit) && typeof c.sourceDirty === 'boolean', 'Invalid release metadata');
  const version = ({'v1.01':'1.0.1','v1.02':'1.0.2'})[c.releaseTag] ?? c.releaseTag.slice(1).split('-')[0];
  insist(version === c.applicationVersion, 'Release version mismatch');
  insist(Array.isArray(c.parts) && c.parts.length === 3, 'Exactly three split images required');
  let previous = 0;
  c.parts.forEach((p, i) => {
    const [role, path, offset, maximum] = specs[i];
    insist(exact(p, 'role path offset size sha256') && p.role === role && p.path === path &&
      p.offset === offset && Number.isSafeInteger(p.size) && p.size > 0 && p.size <= maximum &&
      /^[0-9a-f]{64}$/.test(p.sha256), 'Invalid image metadata/path');
    insist(i !== 1 || p.size === 3072, 'Invalid partition table size');
    const end = Math.ceil((offset+p.size)/4096)*4096;
    insist(offset >= previous && end <= c.flashSize &&
      (end <= 0x9000 || offset >= 0x10000), 'Protected sector overlap');
    previous = end;
  });
  return c;
}

export function validatePartition(data) {
  insist(data instanceof Uint8Array && data.length === 3072, 'Invalid partition table length');
  const expected = [[1,2,0x9000,0x6000,'nvs'], [1,1,0xf000,0x1000,'phy_init'], [0,0,0x10000,0x177000,'factory']];
  expected.forEach(([type,subtype,offset,size,label], i) => {
    const entry = new Uint8Array(32), view = new DataView(entry.buffer);
    view.setUint16(0,0x50aa,true); entry[2]=type; entry[3]=subtype;
    view.setUint32(4,offset,true); view.setUint32(8,size,true); entry.set(new TextEncoder().encode(label),12);
    insist(equal(data.slice(i*32,i*32+32),entry), 'Unsupported partition layout; settings preservation cannot be guaranteed');
  });
  insist(data[96] === 0xeb && data[97] === 0xeb && data.slice(98,112).every(x=>x===255) &&
    hex(data.slice(112,128)) === md5(data.slice(0,96)) && data.slice(128).every(x=>x===255),
  'Invalid partition table checksum/extra partitions');
  return data;
}

export async function validateImage(data, version) {
  insist(data.length >= 48 && data[0] === 0xe9 && data[1] >= 1 && data[1] <= 16 &&
    data[2] === 2 && data[3] === 0x4f && data[12] === 9 && data[13] === 0 && data[23] === 1,
  'Image header is not ESP32-S3 / DIO / 80MHz / 16MB');
  const view = new DataView(data.buffer, data.byteOffset, data.byteLength);
  let pos=24, checksum=0xef;
  for (let i=0;i<data[1];i++) {
    insist(pos+8 <= data.length, 'Truncated image');
    const size=view.getUint32(pos+4,true); pos+=8;
    insist(pos+size <= data.length, 'Truncated segment');
    for (const byte of data.slice(pos,pos+size)) checksum ^= byte;
    pos+=size;
  }
  const end=(Math.floor(pos/16)+1)*16;
  insist(end+32 === data.length && data[end-1] === checksum &&
    data.slice(pos,end-1).every(x=>x===0) && await sha256(data.slice(0,end)) === hex(data.slice(end)),
  'Image checksum/digest mismatch');
  if (version !== undefined) insist(hex(data.slice(32,36)) === '3254cdab' &&
    text(data.slice(48,80)) === version && text(data.slice(80,112)) === 'quadband_cores3',
  'Application descriptor/version mismatch');
}

export async function validatePayload(c, files) {
  validateCatalog(c);
  insist(Array.isArray(files) && files.length === 3, 'Missing images');
  for (let i=0;i<3;i++) {
    const data=files[i], p=c.parts[i];
    insist(data instanceof Uint8Array && data.length === p.size && await sha256(data) === p.sha256,
      `SHA-256 mismatch: ${p.role}`);
    if (i === 1) validatePartition(data);
    else await validateImage(data, i === 2 ? c.firmwareVersion : undefined);
  }
}

export function validateSecurity(info) {
  insist(info && info.chipId === 9 && info.apiVersion === 0 && info.flags === 0 &&
    info.flashCryptCnt === 0 && Array.isArray(info.keyPurposes) && info.keyPurposes.length === 7 &&
    info.keyPurposes.every(x=>x===0), 'Security locked or unknown configuration; installation refused');
}

export function compatiblePartition(current, released) {
  insist(current instanceof Uint8Array && current.length === 3072, 'Cannot inspect current partitions');
  if (current.every(x=>x===255)) return 'blank';
  validatePartition(current);
  insist(equal(current,released), 'Existing partition table differs; migration is not supported');
  return 'supported';
}

export async function boundedBody(response, limit, expected, signal) {
  const declared=response.headers?.get('content-length');
  if (declared !== undefined && declared !== null) {
    const size=Number(declared);
    insist(Number.isSafeInteger(size) && size >= 0 && size <= limit &&
      (expected === undefined || size === expected), 'Download size mismatch');
  }
  insist(response.body?.getReader, 'Streaming firmware downloads are required');
  const reader=response.body.getReader(), chunks=[];
  let size=0;
  try {
    for (;;) {
      insist(!signal?.aborted, 'Firmware download cancelled');
      const {value,done}=await reader.read();
      if (done) break;
      insist(value instanceof Uint8Array, 'Invalid download chunk');
      size+=value.length;
      insist(size <= limit, 'Download exceeds allowed size');
      chunks.push(value);
    }
    insist(expected === undefined || size === expected, 'Download size mismatch');
    const data=new Uint8Array(size);
    let offset=0;
    for (const chunk of chunks) {data.set(chunk,offset);offset+=chunk.length;}
    return data;
  } catch(error) {
    try {await reader.cancel();} catch { /* The network may already be closed. */ }
    throw error;
  } finally {reader.releaseLock();}
}

export async function fetchPayload(base, fetcher=fetch, signal) {
  const root=new URL('./',base);
  const load=async path => {
    const url=new URL(path,root);
    insist(url.origin === root.origin && url.pathname.startsWith(root.pathname), 'Nonlocal payload');
    const response=await fetcher(url,{cache:'no-store',signal,redirect:'error'});
    insist(response.ok, `Released firmware unavailable (${response.status})`);
    return response;
  };
  const bytes=await boundedBody(await load('catalog.json'),32768,undefined,signal);
  const c=validateCatalog(JSON.parse(new TextDecoder('utf-8',{fatal:true}).decode(bytes)));
  const files=[];
  for (const part of c.parts) {
    const response=await load(part.path);
    files.push(await boundedBody(response,part.size,part.size,signal));
  }
  await validatePayload(c,files);
  return {catalog:c,files};
}
