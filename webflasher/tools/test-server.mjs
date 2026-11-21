import {createServer} from 'node:http';
import {readFile} from 'node:fs/promises';
import {resolve,sep,extname} from 'node:path';

const root=resolve(import.meta.dirname,'../dist');
const payload=process.env.QB_REAL_PAYLOAD ? resolve(process.env.QB_REAL_PAYLOAD) : root;
const prefix='/Quadband_Card_Studio/';
const types={'.html':'text/html','.js':'text/javascript','.css':'text/css','.json':'application/json',
  '.txt':'text/plain','.bin':'application/octet-stream','.svg':'image/svg+xml','.png':'image/png'};
createServer(async(req,res)=>{
  try {
    const path=new URL(req.url,'http://127.0.0.1').pathname;
    if (!path.startsWith(prefix)) {res.writeHead(404);res.end();return;}
    const relative=decodeURIComponent(path.slice(prefix.length)) || 'index.html';
    const base=relative === 'catalog.json' || relative === 'BUILD-INFO.json' || relative === 'SHA256SUMS.txt' ||
      relative.startsWith('firmware/') ? payload : root;
    const target=resolve(base,relative);
    if (!target.startsWith(base+sep)) {res.writeHead(403);res.end();return;}
    const data=await readFile(target);
    res.writeHead(200,{'content-type':types[extname(target)] || 'application/octet-stream',
      'content-length':data.length,'cache-control':'no-store'});
    res.end(data);
  } catch {res.writeHead(404);res.end();}
}).listen(4178,'127.0.0.1');
