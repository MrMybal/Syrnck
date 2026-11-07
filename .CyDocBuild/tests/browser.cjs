// Optional real-browser verification. Install Playwright separately or set
// SYRNCK_PLAYWRIGHT to an existing package; no browser dependency ships in help.
const { chromium } = require(process.env.SYRNCK_PLAYWRIGHT || 'playwright');
const assert = require('node:assert/strict');
const { readFileSync, mkdirSync } = require('node:fs');
const { resolve, join } = require('node:path');
const { pathToFileURL } = require('node:url');

(async () => {
  const directory = resolve(process.argv[2] || join(__dirname, '../../Documentation'));
  const manifest = JSON.parse(readFileSync(join(directory, 'DocData/manifest.json'), 'utf8'));
  const url = pathToFileURL(join(directory, 'Documentation.html')).href;
  const browser = await chromium.launch({ headless: true });
  const errors = [], network = [];
  const observe = page => {
    page.on('pageerror', error => errors.push(error.message));
    page.on('console', message => { if (message.type() === 'error') errors.push(message.text()); });
    page.on('request', request => { if (/^https?:/.test(request.url())) network.push(request.url()); });
  };
  try {
    const context = await browser.newContext({ offline: true, viewport: { width: 1365, height: 900 } });
    const page = await context.newPage(); observe(page);
    await page.goto(url);
    await page.locator('main article').waitFor();
    assert.equal(await page.locator('html').getAttribute('lang'), 'en');
    await page.keyboard.press('Tab');
    assert.equal(await page.evaluate(() => document.activeElement.className), 'skip-link');
    await page.keyboard.press('Enter');
    assert.equal(await page.evaluate(() => document.activeElement.id), 'main-content');

    for (const entry of manifest.pages) {
      await page.evaluate(hash => { location.hash = hash; }, `#/${entry.locale}/${entry.id}`);
      await page.waitForFunction(title => document.title === title, `${entry.title} — Syrnck`);
      const missing = await page.locator('article.prose').evaluate((element, ids) =>
        ids.filter(id => ![...element.querySelectorAll('[id]')].some(node => node.id === id)),
        [...entry.headings.map(h => h.id), ...(entry.anchors || [])]);
      assert.deepEqual(missing, [], entry.id);
    }
    const navigate = async (locale, id, anchor) => {
      const entry = manifest.pages.find(p => p.id === id && p.locale === locale);
      await page.evaluate(hash => { location.hash = hash; }, `#/${locale}/${id}${anchor ? '/' + anchor : ''}`);
      await page.waitForFunction(title => document.title === title, `${entry.title} — Syrnck`);
    };
    await navigate('en', 'native-checkout');
    await page.evaluate(() => window.scrollTo(0, 1100));
    const readingPosition = await page.evaluate(() => window.scrollY);
    await navigate('en', 'overview');
    await navigate('en', 'native-checkout');
    await page.waitForFunction(top => Math.abs(window.scrollY - top) < 3, readingPosition);
    await navigate('en', 'packs', 'memoire-du-gc');
    await page.locator('select[aria-label="Documentation language"]').selectOption('fr');
    await page.waitForFunction(() => document.documentElement.lang === 'fr');
    assert.ok(page.url().endsWith('#/fr/packs/memoire-du-gc'));
    await page.keyboard.press('Control+k');
    const search = page.locator('#documentation-search input');
    await search.fill('checkout');
    await page.locator('.search-result').first().waitFor();
    await search.press('ArrowDown');
    await search.press('Enter');
    await page.waitForFunction(() => !document.querySelector('#documentation-search').open);
    await navigate('en', 'overview');
    await page.locator('.theme-picker summary').click();
    await page.locator('[data-set-theme="dark"]').click();
    assert.equal(await page.locator('#app').getAttribute('data-theme'), 'dark');
    const screenshots = resolve(__dirname, '../.cache/screenshots'); mkdirSync(screenshots, { recursive: true });
    await page.screenshot({ path: join(screenshots, 'desktop.png') });

    const mobile = await browser.newContext({ offline: true, hasTouch: true, isMobile: true,
      viewport: { width: 390, height: 844 } });
    const touch = await mobile.newPage(); observe(touch);
    await touch.goto(url + '#/fr/architecture');
    await touch.locator('main article svg').waitFor();
    await touch.locator('[data-menu]').tap();
    assert.equal(await touch.locator('[data-menu]').getAttribute('aria-expanded'), 'true');
    await touch.locator('.brand').tap();
    await touch.waitForFunction(() => location.hash === '#/fr/overview');
    for (const id of ['architecture', 'protocol', 'usage', 'licenses']) {
      await touch.evaluate(id => { location.hash = '#/fr/' + id; }, id);
      await touch.waitForFunction(title => document.title === title,
        `${manifest.pages.find(p => p.id === id && p.locale === 'fr').title} — Syrnck`);
      assert.ok(await touch.evaluate(() => document.documentElement.scrollWidth <= innerWidth + 1), id);
    }
    await touch.screenshot({ path: join(screenshots, 'mobile.png') });
    assert.deepEqual(network, []);
    assert.deepEqual(errors, []);
    console.log(JSON.stringify({ pages: manifest.pages.length, offline: true, networkRequests: network.length,
      checks: ['rendered anchors', 'keyboard', 'search', 'language anchor', 'reading position', 'theme', 'touch', 'narrow layout'], screenshots }));
  } finally { await browser.close(); }
})().catch(error => { console.error(error); process.exitCode = 1; });
