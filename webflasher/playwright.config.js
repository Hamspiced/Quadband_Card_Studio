import {defineConfig} from '@playwright/test';

export default defineConfig({
  testDir:'./tests/browser',
  workers:1,
  timeout:30000,
  reporter:'list',
  use:{
    baseURL:'http://127.0.0.1:4178/Quadband_Card_Studio/',
    viewport:{width:1440,height:1100},
    colorScheme:'dark',
    launchOptions:process.env.PW_CHROME_PATH ? {executablePath:process.env.PW_CHROME_PATH} : {},
  },
  webServer:{
    command:'node tools/test-server.mjs',
    url:'http://127.0.0.1:4178/Quadband_Card_Studio/',
    timeout:10000,
    reuseExistingServer:false,
  },
});
