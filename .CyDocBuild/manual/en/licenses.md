---
id: licenses
title: License and third-party notices
description: License and third-party notices
section: Syrnck
order: 999
revision: 1
---

## Syrnck license {#license}

Syrnck’s original code is MIT. The following original notice applies.

```text
MIT License

Copyright (c) 2026 MrMybal and Syrnck contributors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## Components and distribution {#dependencies}

Native components retain their individual licenses. The table below preserves the original notice. The installed engine also supplies complete applicable license files under share/licenses/Syrnck. The optional Syncthing Go module is MPL-2.0, downloaded separately; MIT does not relicense it.

The local reader uses CyDoc 0.1.0 (MIT), marked (MIT) and Prism (MIT); esbuild (MIT) builds the reader. Their license files ship in DocData/licenses. The authoring dependency is pinned to the vendored CyDoc archive; the reader loads no remote library. No optional media module or Three.js is included.


| Component | Pinned version | Source | License / notice |
| --- | --- | --- | --- |
| BLAKE3 C implementation | 1.8.7 | `c/` of the official blake3 1.8.7 crate, matching the Rust prototype's hash implementation | Apache-2.0 / CC0; LLVM exception where applicable, included in `third_party/blake3/LICENSE_*` |
| FastCDC 2020 port and gear/mask tables | fastcdc 5.0.0, normalization 1, seed 0 | Nathan Fiedler's fastcdc crate, `v2020::cut_gear` and tables | MIT; `third_party/fastcdc/LICENSE` |
| Monocypher | 4.0.2 | [Official download](https://monocypher.org/download/monocypher-4.0.2.tar.gz), `src/monocypher.c`, `src/monocypher.h` | BSD-2-Clause / CC0; included `LICENCE.md` |
| nlohmann/json | 3.12.0 | [Official release](https://github.com/nlohmann/json/releases/tag/v3.12.0), single-header distribution | MIT; included `LICENSE.MIT` |
| cpp-httplib | 0.28.0 | [Official repository](https://github.com/yhirose/cpp-httplib/tree/v0.28.0), HTTP server only | MIT; included `LICENSE` |
| utf8proc | 2.10.0 | [Official repository](https://github.com/JuliaStrings/utf8proc/tree/v2.10.0) | MIT and Unicode data terms; included `LICENSE.md` |
| PicoSHA2 | `161cb3fc4170fa7a3eca9e582cebd27cc4d1fe29` | [Official repository](https://github.com/okdshin/PicoSHA2/tree/161cb3fc4170fa7a3eca9e582cebd27cc4d1fe29), unmodified `picosha2.h` | MIT; included `third_party/picosha2/LICENSE` |
| Zstd | 1.5.7 | [Official release archive](https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz) | BSD-3-Clause / GPL-2.0 option; licenses in the fetched archive |
| curl / libcurl | 8.16.0 | [Official archive](https://curl.se/download/curl-8.16.0.tar.xz) | curl license; `COPYING` in the fetched archive |
| CyDoc | 0.1.0, `e038c474577e1f305df3eba421f2b9c03b0f1d8d` | [CyDoc](https://github.com/MrMybal/CyDoc), packed archive in `.CyDocBuild/vendor` | MIT; `.CyDocBuild/vendor/CyDoc.LICENSE` |
| marked | 17.0.6 | CyDoc reader dependency, pinned in `.CyDocBuild/package-lock.json` | MIT; reader's `DocData/licenses/marked.txt` |
| Prism | 1.30.0 | CyDoc reader dependency, pinned in `.CyDocBuild/package-lock.json` | MIT; reader's `DocData/licenses/Prism.txt` |
| esbuild | 0.25.12 | Documentation build tool, pinned in `.CyDocBuild/package-lock.json` | MIT; reader's `DocData/licenses/esbuild.txt` |
