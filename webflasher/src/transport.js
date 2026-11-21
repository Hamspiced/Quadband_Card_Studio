import {Transport} from 'esptool-js';

export class SafeTransport extends Transport {
  async write(data) {
    if (this.closed || !this.device.writable) throw new Error('Serial transport is closed');
    const writer=this.device.writable.getWriter();
    try {
      const packet=this.slipWriter(data);
      if (this.tracing) this.trace(`Write ${packet.length} bytes: ${this.hexConvert(packet)}`);
      await writer.write(packet);
    } finally {writer.releaseLock();}
  }
  async disconnect() {
    this.closed=true;
    return super.disconnect();
  }
}
