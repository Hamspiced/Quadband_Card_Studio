import {md5,sha256,specs} from '../src/policy.js';

export async function image(application=false) {
  const data=new Uint8Array(208);
  data.set([0xe9,1,2,0x4f]);data[12]=9;data[23]=1;
  new DataView(data.buffer).setUint32(28,128,true);
  if(application) {
    data.set([0x32,0x54,0xcd,0xab],32);
    data.set(new TextEncoder().encode('0.2.1'),48);
    data.set(new TextEncoder().encode('quadband_cores3'),80);
  }
  let checksum=0xef;
  for(const byte of data.slice(32,160)) checksum^=byte;
  data[175]=checksum;
  data.set(Buffer.from(await sha256(data.slice(0,176)),'hex'),176);
  return data;
}
export function table() {
  const data=new Uint8Array(3072).fill(255),view=new DataView(data.buffer);
  [[1,2,0x9000,0x6000,'nvs'],[1,1,0xf000,0x1000,'phy_init'],[0,0,0x10000,0x177000,'factory']]
    .forEach(([kind,subtype,offset,size,name],i)=>{
      const start=i*32;
      data.fill(0,start,start+32);view.setUint16(start,0x50aa,true);
      data[start+2]=kind;data[start+3]=subtype;view.setUint32(start+4,offset,true);
      view.setUint32(start+8,size,true);data.set(new TextEncoder().encode(name),start+12);
    });
  data.set([0xeb,0xeb],96);data.set(Buffer.from(md5(data.slice(0,96)),'hex'),112);
  return data;
}
export async function payload() {
  const files=[await image(),table(),await image(true)];
  const catalog={schemaVersion:1,board:'M5Stack CoreS3',chipFamily:'ESP32-S3',flashSize:16777216,
    flashMode:'dio',flashFrequency:'80m',applicationVersion:'1.0.2',firmwareVersion:'0.2.1',
    releaseTag:'v1.0.2',sourceCommit:'a'.repeat(40),sourceDirty:false,parts:[]};
  for(const [i,[role,path,offset]] of specs.entries())
    catalog.parts.push({role,path,offset,size:files[i].length,sha256:await sha256(files[i])});
  return {catalog,files};
}
export const security=()=>({chipId:9,apiVersion:0,flags:0,flashCryptCnt:0,keyPurposes:[0,0,0,0,0,0,0]});
