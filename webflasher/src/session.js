import { insist, validatePayload, validateSecurity, compatiblePartition, md5 } from './policy.js';

const sleep = ms => new Promise(resolve=>setTimeout(resolve,ms));
export class VerifiedHardReset {
  constructor(transport, usb=false) { this.transport=transport; this.usb=usb; }
  async reset() {
    const t=this.transport;
    await t.setDTR(false);
    await t.setRTS(true);
    const internalUsb=this.usb || (t.getVid() === 0x303a && t.getPid() === 0x1001);
    await sleep(internalUsb ? 200 : 100);
    await t.setRTS(false);
    if (internalUsb) await sleep(200);
  }
}

export class FlashSession {
  constructor({requestPort, createDriver, notify=()=>{}, timeout=180000}) {
    Object.assign(this,{requestPort,createDriver,notify,timeout});
    this.state='idle'; this.busy=false; this.cancelled=false; this.poisoned=false;
  }
  check() { insist(!this.cancelled, 'Cancelled or disconnected; installation incomplete'); }
  emit(state, detail='') { this.state=state; this.notify(state,detail); }
  async checkLayout(loader, released) {
    const layout=compatiblePartition(await loader.readFlash(0x8000,3072),released); this.check();
    if (layout === 'blank') {
      const blank=new Uint8Array(16777216).fill(255);
      insist(await loader.flashMd5sum(0,blank.length) === md5(blank),
        'Partition table is blank but flash is not fully blank; refusing unknown layout');
      this.check();
    }
    return layout;
  }
  async exclusive(action) {
    insist(!this.busy && !this.poisoned, 'Another operation is active or serial cleanup requires page reload');
    this.busy=true; this.cancelled=false;
    const timer=setTimeout(()=>this.cancel('Operation timed out'),this.timeout);
    try { return await action(); }
    catch (error) {
      this.emit('failed',`${error.message}. No automatic retry. Reconnect explicitly; recovery may require download mode.`);
      await this.cleanup();
      if (this.poisoned) this.emit('failed',`${error.message}. Serial cleanup failed: close or reload this tab before reconnecting.`);
      throw error;
    } finally { clearTimeout(timer); this.busy=false; }
  }
  async inspect(payload, acknowledged) {
    return this.exclusive(async()=>{
      insist(acknowledged, 'Confirm CoreS3 model and host-off/disconnected peripherals first');
      insist(!this.driver && this.state !== 'success', 'Disconnect or reload before another session');
      await validatePayload(payload.catalog,payload.files); this.check();
      this.emit('selecting');
      const port=await this.requestPort(); this.port=port; this.check();
      const {loader,transport}=this.createDriver(port); this.driver=loader; this.transport=transport;
      port.addEventListener?.('disconnect',this.onDisconnect=()=>this.cancel('USB disconnected'));
      this.emit('inspecting');
      await loader.detectChip('default_reset',2); this.check();
      insist(loader.chip?.CHIP_NAME === 'ESP32-S3', 'Wrong chip: ESP32-S3 required');
      validateSecurity(await loader.getSecurityInfo()); this.check();
      insist(!loader.secureDownloadMode, 'Secure download mode not supported');
      await loader.runStub(); this.check();
      insist(loader.IS_STUB === true, 'Verified stub required');
      const id=await loader.readFlashId(); this.check();
      insist(Number.isInteger(id) && id > 0 && id < 0xffffff &&
        [0x18,0x38].includes((id>>>16)&255), 'Wrong or unknown flash capacity: 16MB required');
      const layout=await this.checkLayout(loader,payload.files[1]); this.check();
      this.payload=payload;
      this.emit('ready',`ESP32-S3 · 16 MB · ${layout === 'blank' ? 'blank partition table' : 'supported layout'} · security checks passed`);
      return layout;
    });
  }
  async install(acknowledged) {
    return this.exclusive(async()=>{
      insist(this.state === 'ready' && this.driver && acknowledged, 'Explicit installation approval required');
      this.check();
      const {catalog,files}=this.payload, loader=this.driver;
      await validatePayload(catalog,files); this.check();
      await this.checkLayout(loader,files[1]); this.check();
      this.emit('writing','Do not unplug. Only three firmware regions will be written.');
      let verified=0;
      await loader.writeFlash({
        fileArray:files.map((data,i)=>({data,address:catalog.parts[i].offset})),
        flashMode:'dio',flashFreq:'80m',flashSize:'16MB',eraseAll:false,compress:true,
        calculateMD5Hash:data=>{ this.check(); verified++; return md5(data); },
        reportProgress:(i,written,total)=>{this.check(); this.notify('writing',`${catalog.parts[i].role}: ${Math.floor(written/total*100)}%`);},
      }); this.check();
      insist(verified === 3, 'Driver did not verify every programmed image');
      this.emit('verifying');
      for (let i=0;i<3;i++) {
        insist(await loader.flashMd5sum(catalog.parts[i].offset,files[i].length) === md5(files[i]),
          `Programmed-byte verification failed: ${catalog.parts[i].role}`);
        this.check();
      }
      this.emit('resetting');
      await loader.after('hard_reset'); this.check();
      await this.cleanup();
      this.check();
      insist(!this.poisoned, 'Firmware verified but serial cleanup failed; close this tab');
      this.emit('success','All three regions verified. CoreS3 restarted. NVS / PHY sectors were not written.');
    });
  }
  cancel(reason='Cancelled') {
    this.cancelled=true;
    this.emit('cancelling',`${reason}. Installation may be incomplete; waiting for serial operation to stop. If it remains stuck, close or reload this tab.`);
    // No replacement session is allowed until the outstanding driver promise settles.
    if (this.transport && !this.closing) this.closeTransport();
    if (!this.busy) {
      this.busy=true;
      this.cleanup().finally(()=>{
        this.busy=false;
        this.emit('failed',`${reason}. Reconnect explicitly to inspect again.`);
      });
    }
  }
  closeTransport() {
    const transport=this.transport;
    this.closing=Promise.resolve().then(async()=>{
      // Always release boot-selection lines while the port is still available.
      try { await transport.setDTR?.(false); } catch { /* A removed port may reject signals. */ }
      await transport.disconnect();
    }).catch(()=>{
      this.poisoned=true;
    });
    return this.closing;
  }
  async cleanup() {
    const port=this.port;
    port?.removeEventListener?.('disconnect',this.onDisconnect);
    if (this.transport) {
      let timer;
      await Promise.race([this.closing || this.closeTransport(), new Promise(resolve=>{
        timer=setTimeout(()=>{this.poisoned=true;resolve();},5000);
      })]);
      clearTimeout(timer);
    } else if (port?.readable || port?.writable) {
      try { await port.close(); } catch { this.poisoned=true; }
    }
    this.transport=null; this.driver=null; this.payload=null; this.port=null;
    this.closing=null;
  }
}
