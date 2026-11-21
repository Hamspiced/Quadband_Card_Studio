import {readFile,readdir,mkdir,writeFile} from 'node:fs/promises';
import {resolve,join} from 'node:path';

const root=resolve(import.meta.dirname,'..');
const lock=JSON.parse(await readFile(join(root,'package-lock.json'),'utf8'));
let result='Quadband Card Studio CoreS3 browser installer\nOriginal application: Midwest Gadgets. RF research credits: Tototo31 / Ashsh.\n\nDependency graph (including build and optional platform packages).\nExact versions, sources and integrity hashes are preserved in package-lock.json.\n\n';
result+='Original application Copyright (C) 2026 Midwest Gadgets, GPL-3.0-only.\nSee LICENSE.txt and BRANDING.txt. Matching source archives are attached to the indicated GitHub release.\n\n';
for (const [path,item] of Object.entries(lock.packages)) {
  if (!path) continue;
  result+=`${path} @ ${item.version}\nLicense: ${item.license || 'See package notices'}\nSource: ${item.resolved || 'package-lock.json'}\n`;
  try {
    const folder=join(root,path);
    const names=(await readdir(folder)).filter(n=>/^(license|copying|notice|copyright)([.-]|$)/i.test(n));
    for(const name of names) result+=`\n--- ${path}/${name} ---\n${await readFile(join(folder,name),'utf8')}\n`;
    if (!names.length) throw new Error(`Missing license text: ${path}`);
  } catch(error) {
    // Optional platform binaries can be absent on this machine; their parent distribution contains the license.
    if (!item.optional) throw error;
    result+='Optional platform package not installed; governed by the parent distribution license above.\n';
  }
  result+='\n========================================\n\n';
}
await mkdir(join(root,'dist'),{recursive:true});
await writeFile(join(root,'dist/THIRD-PARTY.txt'),result);
await writeFile(join(root,'dist/LICENSE.txt'),await readFile(join(root,'../LICENSE')));
await writeFile(join(root,'dist/BRANDING.txt'),await readFile(join(root,'../BRANDING.txt')));
