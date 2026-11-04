# Third-party components

Syrnck's implementation is C++20, with native C dependencies. No Rust compiler, Cargo package or Rust runtime is required to build or run it.

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

The small dependencies are vendored. CMake fetches Zstd and curl from versioned archives, verifies SHA-256, and also accepts the identical cached archives in `.tools/downloads`. Their hashes are recorded in `cmake/dependencies.cmake`. Windows uses Schannel for HTTPS; Unix uses OpenSSL 3.x supplied by the build environment. The pinned curl is incompatible with OpenSSL 4 APIs, so the network configuration rejects that major version. OS CSPRNGs provide keys and nonces.

PicoSHA2 is private to the Git LFS adapter and verifies the SHA-256 identities of imported LFS objects. The vendored files are copied verbatim from the pinned commit: `picosha2.h` SHA-256 `b13c180161ffac8d0adc81e033e493c409457c4d1258ab9781ac80579ba3bdd8`; `LICENSE` SHA-256 `6c30eb1f37554ec4199cb82a8c86d9e7a852da78a757832bebb03ac9ddc44ff1`.

The source project still needs its own distribution license. Any release package must include these notices and applicable dependency licenses. No Lore or Syncthing implementation was copied into this project.
