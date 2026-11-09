# Third-party components

Syrnck's original code is distributed under the [MIT License](LICENSE).
The components below retain their own licenses and copyright notices.

Syrnck's implementation is C++20, with native C dependencies. No Rust compiler, Cargo package or Rust runtime is required to build or run it.

| Component | Pinned version | Source | License / notice |
| --- | --- | --- | --- |
| BLAKE3 C implementation | 1.8.7 | `c/` of the official blake3 1.8.7 crate, matching the Rust prototype's hash implementation | Apache-2.0 / CC0; LLVM exception where applicable, included in `third_party/blake3/LICENSE_*` |
| FastCDC 2020 port and gear/mask tables | fastcdc 5.0.0, normalization 1, seed 0 | Nathan Fiedler's fastcdc crate, `v2020::cut_gear` and tables | MIT; `third_party/fastcdc/LICENSE` |
| Monocypher | 4.0.3 | [Official download](https://monocypher.org/download/monocypher-4.0.3.tar.gz), `src/monocypher.c`, `src/monocypher.h` and optional `monocypher-ed25519.c` / `.h` | BSD-2-Clause / CC0; included `LICENCE.md` |
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

The small dependencies are vendored. CMake fetches Zstd and curl from versioned archives, verifies SHA-256, and also accepts the identical cached archives in `.tools/downloads`. Their hashes are recorded in `cmake/dependencies.cmake`. Windows uses Schannel for HTTPS; Unix uses OpenSSL 3.x supplied by the build environment. The pinned curl is incompatible with OpenSSL 4 APIs, so the network configuration rejects that major version. OS CSPRNGs provide keys and nonces.

PicoSHA2 is private to the Git LFS adapter and verifies the SHA-256 identities of imported LFS objects. The vendored files are copied verbatim from the pinned commit: `picosha2.h` SHA-256 `b13c180161ffac8d0adc81e033e493c409457c4d1258ab9781ac80579ba3bdd8`; `LICENSE` SHA-256 `6c30eb1f37554ec4199cb82a8c86d9e7a852da78a757832bebb03ac9ddc44ff1`.

Any release package must include these notices and the applicable dependency
licenses. CMake installation copies the project license, this notice, vendored
license files and the fetched Zstd/libcurl license files to
`share/licenses/Syrnck` (or the configured data directory).

The optional Go adapter in `bindings/syncthing` depends on Syncthing's upstream
protocol library ([pinned module](bindings/syncthing/go.mod), based on v2.1.7).
Syncthing remains an external [MPL-2.0 dependency](https://github.com/syncthing/syncthing/blob/v2.1.7/LICENSE),
with its original notices. Its other Go module dependencies retain their own
licenses. They are prepared separately. Enabling `SYRNCK_BUILD_BEP_SERVICE`
adds the resident `syrnck-bep` executable to the CMake installation. Its builder
uses the actual platform runtime import graph, checks pinned module contents and
retains every external runtime module's unmodified source ZIP with module
version/checksum and SHA-256, root license/notice files and the Go runtime license
under `share/licenses/Syrnck/bep`. This includes Syncthing and other dependencies
with their own terms. Keep this directory with the binary distribution; the
manifest identifies the exact compiled dependency set. No Lore or Syncthing
implementation is vendored into the C++ core;
the MIT project license does not relicense any upstream component.

The optional documentation build uses Node.js; installed help is plain local
HTML, CSS and classic JavaScript and requires no Node.js or server at runtime.
CyDoc, marked and Prism notices accompany the reader, together with the esbuild
build-tool license. No optional media readers or Three.js are distributed.

Monocypher 4.0.3 includes the upstream EdDSA/Ed25519 signing timing fix. The optional module supplies standard Ed25519 with SHA-512; the base EdDSA API uses BLAKE2b. Vendored files are unmodified; the release archive SHA-512 and per-file SHA-256 checksums are recorded in `third_party/monocypher/UPSTREAM.json`. The archive was verified against the checksum published by the official download site before copying these files.
