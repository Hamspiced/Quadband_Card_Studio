import { ESPLoader } from 'esptool-js';
import { SafeTransport } from './transport.js';
import { fetchPayload } from './policy.js';
import { FlashSession, VerifiedHardReset } from './session.js';
import './style.css';

const $=id=>document.getElementById(id);
let payload, supported=false;
const confirmations=()=> $('physical').checked && $('peripherals').checked;
const log=text=>{ $('log').textContent=($('log').textContent+text+'\n').slice(-30000); };
function buttons() {
  $('connect').disabled=!supported || !payload || !confirmations() || session.busy ||
    session.poisoned || !!session.driver || session.state === 'success';
  $('install').disabled=session.state !== 'ready' || session.busy || !$('approve').checked || !confirmations();
  $('cancel').disabled=(!session.busy && !session.driver) || session.state === 'cancelling';
  $('confirmations').disabled=session.busy || !!session.driver;
}
const session=new FlashSession({
  requestPort:()=>navigator.serial.requestPort(),
  createDriver:port=>{
    const transport=new SafeTransport(port,false);
    const loader=new ESPLoader({transport,baudrate:115200,romBaudrate:115200,
      terminal:{clean:()=>{},write:log,writeLine:log},
      resetConstructors:{hardReset:(t,usb)=>new VerifiedHardReset(t,usb)}});
    loader.WRITE_BLOCK_ATTEMPTS=1;
    return {loader,transport};
  },
  notify:(state,detail)=>{
    $('state').textContent=state; $('message').textContent=detail || ({
      selecting:'Choose your CoreS3 serial port. No firmware is written during inspection.',
      inspecting:'Checking chip, security, flash capacity and existing partitions…',
      verifying:'Verifying all programmed bytes on the device…',
      resetting:'Verification passed. Restarting CoreS3…',
    })[state] || state;
    if (detail) log(`${state}: ${detail}`);
    $('inspection').hidden=state !== 'ready';
    if (state === 'ready') {$('device-result').textContent=detail;$('approve').checked=false;}
    if (state === 'failed') {$('approve').checked=false;$('progress').value=0;}
    if (state === 'success') $('progress').value=100;
    if (state === 'writing') {
      const match=detail.match(/(\d+)%/);
      if (match) $('progress').value=Number(match[1]);
    }
    const step=state === 'ready' || state === 'inspecting' ? 1 :
      ['writing','verifying','resetting','success'].includes(state) ? 2 : 0;
    document.querySelectorAll('.steps li').forEach((li,i)=>li.classList.toggle('active',i===step));
    buttons();
  },
});
async function action(work) {try {await work();} catch(e) {log(e.message);} finally {buttons();}}
$('connect').onclick=()=>action(()=>session.inspect(payload,confirmations()));
$('install').onclick=()=>action(()=>session.install($('approve').checked && confirmations()));
$('cancel').onclick=()=>{session.cancel();buttons();};
['physical','peripherals','approve'].forEach(id=>$(id).onchange=buttons);
$('theme').onclick=()=>{document.documentElement.dataset.theme=document.documentElement.dataset.theme === 'light' ? 'dark':'light';};
window.addEventListener('beforeunload',event=>{if(session.busy || session.driver){event.preventDefault();event.returnValue='';}});
navigator.serial?.addEventListener?.('disconnect',event=>{if(event.target === session.port) session.cancel('USB disconnected');});

supported=window.isSecureContext && typeof navigator.serial?.requestPort === 'function' && !!globalThis.crypto?.subtle;
$('support').textContent=supported ?
  'Web Serial is available. Port access and firmware installation are always separate, explicit actions.' :
  'This browser cannot install firmware. Open this page over HTTPS in desktop Chrome or Edge, or use the release firmware bundle below.';
$('support').classList.toggle('warning',!supported);
const abort=new AbortController();
const timer=setTimeout(()=>abort.abort(),30000);
try {
  payload=await fetchPayload(new URL('./',document.baseURI),fetch,abort.signal);
  const c=payload.catalog;
  $('source-link').href=`https://github.com/Hamspiced/Quadband_Card_Studio/tree/${c.sourceCommit}`;
  $('source-link').textContent=c.sourceDirty ? 'Base commit (development)' : 'Exact source commit';
  $('release-title').textContent=`Studio ${c.releaseTag}`;
  $('release-status').textContent=c.sourceDirty ? 'DEVELOPMENT SNAPSHOT — uncommitted source / SDK. Not a published release. Never deploy this payload.' :
    c.releaseTag.includes('-') ? 'PRERELEASE — preview firmware. Not the stable release.' : 'Release payload verified · SHA-256 checks passed';
  $('release-status').classList.toggle('warning',c.sourceDirty || c.releaseTag.includes('-'));
  for(const [key,value] of [['Firmware',c.firmwareVersion],['Application',c.applicationVersion],['Source commit',c.sourceCommit],['Target',`${c.chipFamily} · 16 MB`]]) {
    const dt=document.createElement('dt'),dd=document.createElement('dd');dt.textContent=key;dd.textContent=value;
    $('metadata').append(dt,dd);
  }
  for(const part of c.parts) {
    const a=document.createElement('a'),code=document.createElement('code'),p=document.createElement('p');
    a.href=`./${part.path}`;a.download='';a.textContent=`${part.role} · 0x${part.offset.toString(16)} · ${part.size.toLocaleString()} bytes`;
    code.textContent=part.sha256;p.append(a,code);$('downloads').append(p);
  }
  $('state').textContent='Ready to prepare';
} catch(error) {
  payload=null;$('release-title').textContent='Installer unavailable';
  $('release-status').textContent=`Firmware catalog missing or invalid. ${error.message}`;
  $('release-status').classList.add('warning');
  $('state').textContent='Blocked';
  $('message').textContent='No port access or writes are possible without a complete, verified release payload.';
} finally {clearTimeout(timer);buttons();}
