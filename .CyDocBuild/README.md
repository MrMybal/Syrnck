# Syrnck documentation maintenance

This private authoring workspace builds the public offline manual. It is not
copied into the installed documentation. Human content belongs in `manual/en`
and `manual/fr`; maintenance reports belong in `internal/records`.

## Regenerate and verify

Node.js 22+ and npm are build prerequisites. From the repository root:

```text
npm --prefix .CyDocBuild ci --no-audit --no-fund --cache .CyDocBuild/.cache/npm
npm --prefix .CyDocBuild run build
npm --prefix .CyDocBuild run check
npm --prefix .CyDocBuild test
```

`check` also writes the export, as specified by CyDoc. `build.mjs` extracts
explicit public-header annotations, invokes CyDoc's declared CLI/exporter,
installs the host reader template and licenses, and validates bilingual help
targets. Never edit generated HTML, data.js or manifest.json.

The normal CMake build enables `SYRNCK_BUILD_DOCUMENTATION` when the CLI is
enabled, installs npm's locked dependencies, generates `Documentation/`, and
copies the entire export beside the executable even when only docs changed.
`cmake --install build --config Release --prefix dist` installs it under
`dist/bin/Documentation`. Library-only builds default to documentation off;
`-DSYRNCK_BUILD_DOCUMENTATION=OFF` explicitly omits it and needs no Node.
The CLI reports missing help clearly when documentation is omitted.

Initial dependency preparation can require Internet. Installed help uses only
file:// classic scripts, local styles/assets and CyDoc's resource provider;
it has no fetch-based bootstrap, CDN, web server or Node runtime.

## Authoring and translations

Use common stable lowercase page IDs and explicit ASCII heading IDs in both
languages. English is the source/default. French must cover every page. Bump
the English `revision` when changing meaning, review/update its translation,
and set French `sourceRevision` to the reviewed revision. Build fails on
different heading/code anchors or stale/missing translations. Navigation parent
and order must agree. `translation-report.json` is a build report, never a human
navigation page.

Use `doc:page-id#section-id` for local links. `topics.json` maps CLI command
contexts and legacy paths/fragments to canonical destinations. `routes.json`
records migration provenance and old-anchor mappings; it is not a runtime
source browser. The small old Markdown stubs preserve repository links and
point to the canonical manual. Do not maintain duplicate guides there.

Public C++/C documentation uses CyDoc's real `cydoc/extract` API with an explicit
distributed-header allowlist. It does not infer C++ signatures, Go semantics or
binding ownership. The extracted API pages have reviewed English digests
in `api-translations.json`; update those digests only after reviewing the French
annotation. Implementation sources, tests and private indexes are not exported.
Go and BEP contracts stay in authored integration guides.

The application has no existing logo, GUI help menu or F1 action. The CLI adds
`help [TOPIC] [--section ID] [--directory PATH] [--print-url]`, including two-word
command contexts, while terminal `--help` stays available. English/French
selection follows normal CLI precedence. Do not invent a GUI integration.

No optional media module is enabled: the content has prose, tables, code and
one compiled SVG architecture diagram. There are no image/video/GIF/mesh/gallery
assets or interactive node graphs to justify those dependencies. Three.js is
absent. The standard adaptive theme, theme picker, responsive navigation,
keyboard controls, search, and reading-state behavior come from CyDoc.

## Browser and installed-layout tests

The optional browser check uses a separately available Playwright package and
Chromium; neither ships with the application. Set `SYRNCK_PLAYWRIGHT` to that
package path when it is not resolvable as `playwright`. Configure
`PLAYWRIGHT_BROWSERS_PATH` if using a private browser cache, then run:

```text
node .CyDocBuild/tests/browser.cjs
python tests/documentation_help.py build/Release/syrnck.exe
```

On Unix use `build/syrnck`. The browser script opens file:// with network disabled
and checks every rendered page/anchor, search, keyboard, language links, reading
positions, themes and narrow touch layouts. Screenshots stay under ignored
`.cache/screenshots`. An optional argument selects another exported folder.
CTest includes the dependency-free Node/manual and native CLI routing tests.

Generated help aliases must be unique, including JSON-escaped spellings of the
same key. The native CLI rejects ambiguous catalogs before selecting a route.
Its decoder builds this lookup table without repeated linear alias insertion.
The reader/export format and normal page/section URLs remain unchanged.

Windows x64 is the locally exercised platform. Linux/macOS CI runs build,
manual, CLI routing and installation tests when dispatched; configuring that
matrix is not evidence that those jobs have run. Native default-browser launch
depends on the installed platform's browser association; URL generation and
actual file:// rendering are tested separately.

## CyDoc provenance and updates

The local archive is CyDoc 0.1.0 from commit
`e038c474577e1f305df3eba421f2b9c03b0f1d8d`. It was built and packed from an
authenticated Git checkout. `vendor/README.txt` records its provenance and
checksum. The archive and package-lock avoid requiring access to that private
repository for application builds. Runtime licenses are in DocData/licenses.

For an update, read the new CyDoc `ai/README.md`, `ai/INTEGRATION.md`,
`ai/AUTHORING.md`, `ai/API.md` and update guide, pack the actual new revision,
replace the vendored package/lock, rebuild and repeat these checks. The CyDoc
repository's AGENTS instructions govern edits inside CyDoc, not Syrnck.

Keep reports, caches, personal paths and test fixtures out of the human manual.
Run the repository's secret scanner before a local commit or publication;
archive contents and generated reader data require inspection too.

## Binding validation commands

After building the native library, developers can validate the optional integrations:

```text
python scripts/test_go.py --library build/Release/syrnck_ffi.dll
python scripts/test_go.py --library build/Release/syrnck_ffi.dll --module syncthing --check-pointers
python scripts/test_go.py --library build/Release/syrnck_ffi.dll --module syncthing --benchmark
```

Use Unix library locations on Unix. Prepare the adapter’s pinned dependencies with
its module cache first. `SYRNCK_TEST_GO=OFF` disables core Go CTest checks;
`SYRNCK_TEST_SYNCTHING=ON` enables the optional adapter suite.

## System vault qualification

`SYRNCK_SYSTEM_KEY_VAULT=AUTO|ON|OFF` selects native provider build support.
`SYRNCK_TEST_SYSTEM_VAULT=ON` explicitly enables the real-provider CTest fixture;
run `python scripts/build.py test -R "^key_vault_(references|system)$"` after
building. This fixture creates disposable OS credentials, uses separate native
processes and removes its selected entries. Linux needs libsecret development
files, gnome-keyring, dbus-run-session and gdbus; it launches a separate session
bus and keyring. macOS selects a temporary keychain, and Windows needs a usable
account logon session. The dedicated `System key vaults` workflow also qualifies
relocated SDK consumers with the provider enabled and disabled on all three OSes.
`SYRNCK_TEST_SYSTEM_VAULT_GO=ON` adds a Go test binary built with strict pointer
checks and separate create/verify/erase processes; it requires Go locally.
The real fixture covers C++, C shared/static and Go owners. macOS file-keychain
hosts explicitly opt into process-wide noninteractive Keychain policy before
Keychain workers; ordinary vault calls refuse missing policy without changing it.
The fixture verifies this refusal and locked-keychain failure, then unlocks its
own disposable keychain before verification/cleanup. No actual user keychain is
locked or selected as the default.
