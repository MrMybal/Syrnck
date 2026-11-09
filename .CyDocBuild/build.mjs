import { readFile, writeFile, mkdir, readdir, cp, rm, lstat } from 'node:fs/promises';
import { resolve, dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { execFileSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { extractComments } from 'cydoc/extract';
import { createDocumentation, hasAnchor } from 'cydoc';

const base = dirname(fileURLToPath(import.meta.url)), root = dirname(base);
const packageRoot = dirname(dirname(fileURLToPath(import.meta.resolve('cydoc'))));
const config = JSON.parse(await readFile(join(base, 'cydoc.config.json'), 'utf8'));
if (config.modules.length) throw new Error('This text-only manual does not need optional media modules');
// Remove only the private, fixed generated workspace. Never clean user exports.
const generated = join(base, 'generated');
if (dirname(generated) !== base || resolve(generated) !== generated)
  throw new Error('Generated workspace must be an absolute child of the authoring directory');
const previous = await lstat(generated).catch(error => {
  if (error.code !== 'ENOENT') throw error;
});
if (previous?.isSymbolicLink()) throw new Error('Refusing to remove a linked generated workspace');
await rm(generated, { recursive: true, force: true });
await mkdir(generated, { recursive: true });
await cp(join(base, 'manual'), join(generated, 'manual'), { recursive: true });
for (const locale of ['en', 'fr']) for (const name of await readdir(join(generated, 'manual', locale))) {
  const source = await readFile(join(generated, 'manual', locale, name), 'utf8');
  const prose = source.replace(/^```[^\n]*\n[\s\S]*?^```\s*$/gm, '').replace(/`[^`\n]*`/g, '');
  for (const match of prose.matchAll(/!?\[[^\]\n]*\]\(([^)]+)\)/g))
    if (!/^(?:https?:\/\/|mailto:|doc:|#)/.test(match[1]))
      throw new Error(`Unsupported local Markdown link in ${locale}/${name}: ${match[1]}`);
}
const hash = text => createHash('sha256').update(text).digest('hex');
const reviewed = JSON.parse(await readFile(join(base, 'api-translations.json'), 'utf8'));
const headers = ['include/syrnck.h', 'include/syrnck/core.hpp', 'include/syrnck/backup.hpp',
                 'include/syrnck/workspace.hpp', 'include/syrnck/retention.hpp', 'include/syrnck/keys.hpp', 'include/syrnck/signing_keys.hpp'];
const annotations = [];
for (const header of headers) annotations.push(...extractComments(await readFile(join(root, header), 'utf8'), header));
for (const page of annotations) {
  const original = annotations.find(p => p.id === page.id && p.locale === 'en');
  if (!original) throw new Error(`Missing English API annotation: ${page.id}`);
  const revision = hash(original.markdown);
  if (reviewed[page.id.replace(/^api-/, '')]?.englishSha256 !== revision)
    throw new Error(`Review the French API translation after changing: ${page.id}`);
  const metadata = ['---', `id: ${page.id}`, `title: ${page.title}`, `description: ${page.title}`,
                    'section: API', 'parent: integrations', `order: ${page.order}`, 'kind: api',
                    `symbol: ${page.symbol || page.title}`, `source: ${page.source}`,
                    page.locale === 'en' ? `revision: ${revision}` : `sourceRevision: ${revision}`, '---', ''];
  await writeFile(join(generated, 'manual', page.locale, `${page.id}.md`), metadata.join('\n') + page.markdown + '\n');
}
// Use CyDoc's declared CLI and exporter, including its local classic-script bootstrap.
const pkg = JSON.parse(await readFile(join(packageRoot, 'package.json'), 'utf8'));
execFileSync(process.execPath, [join(packageRoot, pkg.bin.cydoc), process.argv.includes('--check') ? 'check' : 'build',
                              join(base, 'cydoc.config.json')], { cwd: root, stdio: 'inherit' });
const output = resolve(base, config.out);
await cp(join(base, 'reader.html'), join(output, 'Documentation.html'));
const manifest = JSON.parse(await readFile(join(output, 'DocData/manifest.json'), 'utf8'));
const docs = createDocumentation(manifest);
if (docs.availableLocales().map(l => l.code).join(',') !== 'en,fr') throw new Error('Both editions must be complete');
const en = docs.pages('en'), fr = docs.pages('fr');
for (const original of en) {
  const translation = fr.find(p => p.id === original.id);
  const anchors = page => [...new Set([...page.headings.map(h => h.id), ...(page.anchors || [])])].sort().join('\n');
  if (!translation || anchors(original) !== anchors(translation)) throw new Error(`Different translated anchors: ${original.id}`);
  if (translation.sourceRevision !== original.revision) throw new Error(`Untracked translation: ${original.id}`);
}
const topics = JSON.parse(await readFile(join(base, 'topics.json'), 'utf8'));
for (const [topic, target] of Object.entries(topics)) {
  const [page, anchor] = target.split('#');
  for (const locale of ['en', 'fr']) {
    const resolved = docs.resolve(page, locale);
    if (!resolved || (anchor && !hasAnchor(resolved.page, anchor))) throw new Error(`Invalid help topic: ${topic}`);
  }
}
await writeFile(join(output, 'DocData/help-topics.json'), JSON.stringify(topics, null, 2) + '\n');
await mkdir(join(output, 'DocData/licenses'), { recursive: true });
for (const [name, path] of Object.entries({ 'CyDoc.txt': 'vendor/CyDoc.LICENSE',
  'marked.txt': 'node_modules/marked/LICENSE.md', 'Prism.txt': 'node_modules/prismjs/LICENSE',
  'esbuild.txt': 'node_modules/esbuild/LICENSE.md' })) await cp(join(base, path), join(output, 'DocData/licenses', name));
for (const path of await readdir(join(output, 'DocData'))) if (/^media-/.test(path)) throw new Error('Unexpected media bundle');
console.log(`Syrnck: ${en.length} bilingual pages; local help topics and translation anchors verified.`);
