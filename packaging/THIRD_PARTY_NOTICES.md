# Third-party notices

TodoBench's own code is GPL-3.0-or-later. Third-party code and artwork retain
 their respective licenses. License texts are installed with each package;
platform dependency-source archives carry upstream sources, licenses, build
recipes, and provenance for the specific native build.

| Component | License | Use |
| --- | --- | --- |
| mdbase-rs 0.4.0-rc.4 (vendored at `native/mdbase_bridge/vendor/mdbase-rs`, pin `ca71ae`) + mdbase spec 0.3.0 (`68b9a9`) | MIT (spec CC-BY-4.0 where applicable) | Offline local mdbase collection core for TodoBench transfer (TodoBench links the compiled staticlib; spec text not copied) |
| Rust crates locked in `native/mdbase_bridge/Cargo.lock` | Each crate's declared license | Statically linked mdbase bridge dependencies; dependency-source archives include the locked manifest, complete vendored registry, local mdbase-rs source, and `rust/dependencies.json` with package source and license metadata |
| @callumalpass/mdbase 0.3.0-rc.5 (vendored independent oracle at `native/mdbase_oracle/ts`, pin `885c4f`) | MIT | Development-only independent mdbase verification engine for CTest `mdbase_oracle_interop` (not shipped/bundled) |
| Qt 6.8.3 (qtbase, qtsvg) | LGPL-3.0 / GPL-3.0 with applicable exceptions | Dynamically linked UI and SVG rendering |
| ICU 73.2 (Qt Linux SDK) | Unicode / ICU terms in bundled license | Unicode support |
| yaml-cpp | MIT | YAML front matter |
| libarchive | BSD and included component notices in COPYING | 7-Zip snapshots |
| cmark-gfm (when available) | BSD-2-Clause and included notices | Markdown structure checks |
| Lucide 0.468.0 | ISC; Feather-derived icons MIT | Vendored SVG subset |

Full Lucide/Feather attribution is in `native/resources/icons/lucide/LICENSE`
in source and `doc/TodoBench/lucide/LICENSE` in installed distributions. The
subset is pinned at f12b0de177fbc2a6795e99be065887e72b237123.

Qt's embedded third-party notices are in its accompanying source archive. Native
transitive dependencies (compression, crypto, image and text libraries) vary by
platform; their exact sources and notices accompany the corresponding binary in
`*-dependency-source.tar.gz`. Linux records Ubuntu source-package versions;
Windows includes the pinned vcpkg recipes and downloaded source archives; macOS
includes Homebrew formulae, source checksums, and installed metadata.

The unmodified dependency sources retain their upstream licenses. Qt is linked
dynamically, and compatible replacement libraries can be used in the unpacked
portable package or application bundle. The application has no publisher-signing
restriction on modified builds. See `packaging.md` for build instructions.
