# Documentation integration notes for coding assistants

This file is for assistants working on the application's documentation. The human manual opens through Documentation.html. Do not import this file into its content, navigation or search index.

```json
{
  "application": "Syrnck",
  "version": "0.1.0",
  "entry": "Documentation.html",
  "manifest": "DocData/manifest.json",
  "defaultLocale": "en",
  "publishedLocales": [
    "en",
    "fr"
  ],
  "modules": []
}
```

## Human reader first

Write the manual for people using the application: clear tasks, useful examples, readable media captions and accessible controls. Preserve the application's own terminology and logo. Show complete labels in tooltips when space requires truncation.

Keep documentation build instructions, AI instructions, migration inventories, audit results and verification history outside the human manual. Store assistant guidance in this DocAI.md sidecar, and keep private working records in the source repository. Mark existing internal Markdown with audience: internal or audience: ai to exclude it from an export.

## Languages

English is the source/default language; a complete French translation is mandatory. Keep the same page IDs and explicit anchors across translations. Translate prose, titles, descriptions, captions, annotations and media explanations; code and API identifiers retain their proper spelling. Do not copy original-language prose into another locale, use placeholders, or declare a partially translated manual complete.

The build fails before publishing when English or French has missing/empty pages or explicitly outdated sourceRevision values. Additional incomplete locales are withheld. This structural check cannot judge translation quality: read the translations before publishing. Do not restore a mixed-language fallback banner or expose incomplete languages in the selector.

## Files and integration

Ship Documentation.html, this DocAI.md, and the complete DocData directory together. The HTML is the reader itself; all runtime scripts, media, styles, notices, manifests and optional version exports are inside DocData. Open the HTML through file:// with no web server, runtime CDN or remote font.

Source Markdown, asset inputs, tooling and the build configuration belong in App/.CyDocBuild/. The CLI looks for .CyDocBuild/cydoc.config.json by default; its output is the sibling App/Documentation/. Edit those inputs and rebuild. Do not edit generated data.js or manifest.json by hand. Authored media and logos use logical assets/ paths; do not prepend DocData in the source content. Custom readers use context.assetUrl for display and retain logical paths for binary providers and annotation identities.

Review documentation against the application's current UI, behavior and public APIs, then correct obsolete descriptions. Organize topics and subpages as a tree. Public function APIs, signatures, includes, imports and modules may be documented; never publish raw source listings or implementation bodies. apiIndex creates API-only references; the legacy sourceIndex option does not restore source browsing. Keep source trees outside the manual's assets.

Select only useful optional modules. A gallery also needs each item's reader; only mesh requires Three.js. Keep image enlargement inside the documentation window and preserve keyboard, touch and Escape controls.

Use Documentation.html#/LOCALE/PAGE/ANCHOR for contextual help, with correctly encoded stable IDs. Preserve existing deep links or add validated aliases. Legacy index.html files may remain solely as compatibility redirects. Test the exported manual and the application's help entry on the target platform; report only checks actually performed.
