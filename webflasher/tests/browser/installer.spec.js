import {test,expect} from '@playwright/test';
import {readFile} from 'node:fs/promises';
import {join} from 'node:path';
import {payload} from '../fixtures.mjs';

async function mockPayload(page, mode='valid', dirty=false) {
  const value=await payload();
  value.catalog.sourceDirty=dirty;
  await page.route('**/Quadband_Card_Studio/catalog.json',route=>mode === 'missing' ?
    route.fulfill({status:404,body:'not available'}) :
    route.fulfill({contentType:'application/json',body:JSON.stringify(value.catalog)}));
  for (let i=0;i<value.files.length;i++) {
    const data=Buffer.from(value.files[i]);
    if (mode === 'corrupt' && i === 2) data[40]^=1;
    await page.route(`**/Quadband_Card_Studio/${value.catalog.parts[i].path}`,
      route=>route.fulfill({contentType:'application/octet-stream',body:data}));
  }
}

async function fakeSerial(page, pending=false) {
  await page.addInitScript(({pending})=>{
    window.testPortRequests=0;
    const fake=new EventTarget();
    fake.requestPort=()=>{
      window.testPortRequests++;
      if (pending) return new Promise((resolve,reject)=>{window.rejectPortPicker=reject;});
      return Promise.reject(new DOMException('Picker cancelled','NotFoundError'));
    };
    Object.defineProperty(navigator,'serial',{configurable:true,value:fake});
  },{pending});
}

test('valid page requires both physical safety acknowledgements; picker cancellation never retries',async({page})=>{
  await mockPayload(page);await fakeSerial(page);
  const errors=[];page.on('pageerror',error=>errors.push(error.message));
  await page.goto('./');
  await expect(page.locator('#release-title')).toHaveText('Studio v1.0.2');
  await expect(page.locator('#connect')).toBeDisabled();
  await page.locator('#physical').check();
  await expect(page.locator('#connect')).toBeDisabled();
  await page.locator('#peripherals').check();
  await expect(page.locator('#connect')).toBeEnabled();
  await expect(page.locator('#install')).toBeDisabled();
  expect(await page.evaluate(()=>window.testPortRequests)).toBe(0);
  await page.locator('#connect').click();
  await expect(page.locator('#state')).toHaveText('failed');
  await expect(page.locator('#connect')).toBeEnabled();
  await expect(page.locator('#install')).toBeDisabled();
  expect(await page.evaluate(()=>window.testPortRequests)).toBe(1);
  await page.waitForTimeout(100);
  expect(await page.evaluate(()=>window.testPortRequests)).toBe(1);
  expect(errors).toEqual([]);
});

for (const mode of ['missing','corrupt']) {
  test(`${mode} firmware blocks port selection`,async({page})=>{
    await mockPayload(page,mode);await fakeSerial(page);
    await page.goto('./');
    await expect(page.locator('#state')).toHaveText('Blocked');
    await page.locator('#physical').check();await page.locator('#peripherals').check();
    await expect(page.locator('#connect')).toBeDisabled();
    expect(await page.evaluate(()=>window.testPortRequests)).toBe(0);
  });
}

test('unsupported browser provides manual guidance without port access',async({page})=>{
  await mockPayload(page);
  await page.addInitScript(()=>Object.defineProperty(navigator,'serial',{configurable:true,value:undefined}));
  await page.goto('./');
  await expect(page.locator('#support')).toContainText('cannot install');
  await page.locator('#physical').check();await page.locator('#peripherals').check();
  await expect(page.locator('#connect')).toBeDisabled();
});
test('insecure context blocks installation even when serial is present',async({page})=>{
  await mockPayload(page);await fakeSerial(page);
  await page.addInitScript(()=>Object.defineProperty(window,'isSecureContext',{configurable:true,value:false}));
  await page.goto('./');
  await expect(page.locator('#support')).toContainText('cannot install');
  await page.locator('#physical').check();await page.locator('#peripherals').check();
  await expect(page.locator('#connect')).toBeDisabled();
  expect(await page.evaluate(()=>window.testPortRequests)).toBe(0);
});

test('cancelling an unresolved picker does not permit another concurrent selection',async({page})=>{
  await mockPayload(page);await fakeSerial(page,true);
  await page.goto('./');await expect(page.locator('#release-title')).toHaveText('Studio v1.0.2');
  await page.locator('#physical').check();await page.locator('#peripherals').check();
  await page.locator('#connect').click();
  await expect(page.locator('#state')).toHaveText('selecting');
  await page.locator('#cancel').click();
  await expect(page.locator('#state')).toHaveText('cancelling');
  await expect(page.locator('#connect')).toBeDisabled();
  expect(await page.evaluate(()=>window.testPortRequests)).toBe(1);
  await page.evaluate(()=>window.rejectPortPicker(new DOMException('Cancelled','AbortError')));
  await expect(page.locator('#state')).toHaveText('failed');
  await expect(page.locator('#connect')).toBeEnabled();
});

test('development label, themes, responsive layout and source credits render',async({page},testInfo)=>{
  await mockPayload(page,'valid',true);await fakeSerial(page);
  await page.goto('./');
  await expect(page.locator('#release-status')).toContainText('DEVELOPMENT SNAPSHOT');
  await expect(page.locator('footer')).toContainText('Midwest Gadgets');
  await expect(page.locator('footer')).toContainText('Tototo31 / Ashsh');
  await page.screenshot({path:testInfo.outputPath('installer-dark.png'),fullPage:true});
  await page.locator('#theme').click();
  await expect(page.locator('html')).toHaveAttribute('data-theme','light');
  await page.screenshot({path:testInfo.outputPath('installer-light.png'),fullPage:true});
  await page.setViewportSize({width:390,height:900});
  await page.waitForTimeout(150);
  await expect(page.locator('header')).toHaveCount(1);
  await expect(page.locator('.hero')).toHaveCount(1);
  expect(await page.evaluate(()=>document.documentElement.scrollWidth <= window.innerWidth)).toBe(true);
  await page.locator('#physical').check();await page.locator('#peripherals').check();
  await expect(page.locator('#connect')).toBeEnabled();
  await page.locator('#connect').click();
  await expect(page.locator('#state')).toHaveText('failed');
  await page.evaluate(()=>window.scrollTo(0,0));
  await page.screenshot({path:testInfo.outputPath('installer-mobile.png')});
});

test('real development export is accepted and visibly labelled',async({page},testInfo)=>{
  test.skip(!process.env.QB_REAL_PAYLOAD,'Optional real firmware export not supplied');
  const catalog=JSON.parse(await readFile(join(process.env.QB_REAL_PAYLOAD,'catalog.json'),'utf8'));
  await fakeSerial(page);
  await page.goto('./');
  await expect(page.locator('#release-status')).toContainText('DEVELOPMENT SNAPSHOT');
  await expect(page.locator('#metadata')).toContainText(catalog.firmwareVersion);
  expect(await page.evaluate(()=>window.testPortRequests)).toBe(0);
  await page.screenshot({path:testInfo.outputPath('installer-real-firmware.png'),fullPage:true});
});
