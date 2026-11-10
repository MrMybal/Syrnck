import test from 'node:test';
import assert from 'node:assert/strict';
import { readFile, readdir } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';
import { resolve, dirname } from 'node:path';
import { createHash } from 'node:crypto';
import { createDocumentation, hasAnchor, parseLink } from 'cydoc';
import { extractComments } from 'cydoc/extract';

const base = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const root = dirname(base), output = resolve(root, 'Documentation');
const json = async path => JSON.parse(await readFile(path, 'utf8'));
const manifest = await json(resolve(output, 'DocData/manifest.json'));
const docs = createDocumentation(manifest);

test('complete English/French navigation and stable sections', () => {
  assert.equal(manifest.defaultLocale, 'en');
  assert.deepEqual(docs.availableLocales().map(l => l.code), ['en', 'fr']);
  const english = docs.pages('en'), french = docs.pages('fr');
  assert.ok(english.length >= 76);
  assert.equal(english.length, french.length);
  for (const page of english) {
    const translated = docs.resolve(page.id, 'fr');
    assert.equal(translated.fallback, false, page.id);
    assert.equal(translated.page.parent, page.parent, page.id);
    assert.equal(translated.page.order, page.order, page.id);
    assert.deepEqual(translated.page.headings.map(h => h.id), page.headings.map(h => h.id), page.id);
    assert.deepEqual(translated.page.anchors, page.anchors, page.id);
    assert.equal(translated.page.sourceRevision, page.revision, page.id);
  }
});

test('every internal link and help topic resolves in both languages', async () => {
  for (const page of manifest.pages) for (const link of page.links || []) {
    if (!link.startsWith('#/')) continue;
    const route = parseLink(link), target = docs.resolve(route.pageId, route.locale);
    assert.ok(target, `${page.id}: ${link}`);
    if (route.anchor) assert.ok(hasAnchor(target.page, route.anchor), `${page.id}: ${link}`);
  }
  const topics = await json(resolve(output, 'DocData/help-topics.json'));
  assert.equal(topics['native tags'], 'native-tags');
  assert.equal(topics['docs/PACKS.md#mémoire-du-gc'], 'packs#memoire-du-gc');
  for (const target of Object.values(topics)) {
    const [id, anchor] = target.split('#');
    for (const locale of ['en', 'fr']) {
      const resolved = docs.resolve(id, locale);
      assert.ok(resolved, target);
      if (anchor) assert.ok(hasAnchor(resolved.page, anchor), target);
    }
  }
  assert.equal(docs.canonicalRoute({ locale: 'fr', pageId: 'NATIVE_TAGS.md' }).pageId, 'native-tags');
});

test('export is local, text-only, with no implementation listings or internal records', async () => {
  const config = await json(resolve(base, 'cydoc.config.json'));
  assert.deepEqual(config.modules, []);
  assert.equal(config.sources, undefined);
  assert.equal(config.apiIndex, undefined);
  const files = await readdir(resolve(output, 'DocData'));
  assert.ok(!files.some(name => name.startsWith('media-')));
  const reader = await readFile(resolve(output, 'Documentation.html'), 'utf8');
  assert.ok(reader.includes("connect-src 'none'"));
  for (const match of reader.matchAll(/(?:src|href)="([^"]+)"/g)) {
    assert.match(match[1], /^DocData\/[a-z.-]+$/);
    await readFile(resolve(output, match[1]));
  }
  const data = await readFile(resolve(output, 'DocData/data.js'), 'utf8');
  assert.ok(!data.includes('three.module') && !data.includes('media-mesh'));
  for (const page of manifest.pages) {
    assert.notEqual(page.kind, 'source');
    assert.ok(!['publishing', 'roadmap', 'translation-report'].includes(page.id));
    assert.ok(!(page.source || '').startsWith('src/'));
    for (const match of page.html.matchAll(/(?:src|poster)="([^"]+)"/g))
      assert.ok(!/^https?:|^\/\//.test(match[1]), page.id);
  }
  for (const name of ['CyDoc.txt', 'marked.txt', 'Prism.txt', 'esbuild.txt'])
    assert.ok((await readFile(resolve(output, 'DocData/licenses', name))).length > 100);
});

test('API extraction uses reviewed public-header annotations only', async () => {
  const reviewed = await json(resolve(base, 'api-translations.json'));
  const extracted = [];
  for (const path of ['include/syrnck.h', 'include/syrnck/core.hpp', 'include/syrnck/keys.hpp', 'include/syrnck/signing_keys.hpp', 'include/syrnck/sealed.hpp', 'include/syrnck/key_vault.hpp'])
    extracted.push(...extractComments(await readFile(resolve(root, path), 'utf8'), path));
  assert.deepEqual(extracted.filter(p => p.locale === 'en').map(p => p.id).sort(), ['api-c-abi', 'api-password-keys', 'api-sealed-repositories', 'api-signing-keys', 'api-store', 'api-system-key-vault']);
  for (const page of extracted.filter(p => p.locale === 'en')) {
    assert.equal(createHash('sha256').update(page.markdown).digest('hex'),
      reviewed[page.id.replace(/^api-/, '')].englishSha256);
    assert.ok(extracted.some(p => p.id === page.id && p.locale === 'fr'));
  }
  const report = await json(resolve(output, 'DocData/translation-report.json'));
  for (const field of ['missing', 'outdated', 'untracked']) assert.deepEqual(report[field], []);
});
