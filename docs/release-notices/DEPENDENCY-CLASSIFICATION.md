# Dependency classification for this notice preparation

Authority order: actual frozen compile/link/package inventory, then selected
source and build rules, then broad upstream prose. Upstream notice copies are
verbatim; they are NOT edited to conceal a contradiction.

| Component | Classification and evidence | Disposition |
|---|---|---|
| UI-kit modules gfx/ui/core/audio/platform/runtime/third_party | compose-kit-root.sh copies them, excludes demo shell and OpenProspero stub | GPL source/notices; preserve shader and stb attribution |
| Font data, sound bank/music | kit assets copied with project overlay; five font license texts retained | font TTF/bake sources present in kit snapshot; kit audio grant in notices |
| Project logo/mesh/previews and sce_sys | project files override upstream presentation | owner grants/source provenance unresolved; kit's grant not projected onto replacements |
| stb_vorbis | src/third_party/stb source linked; UPSTREAM 2c980bb59875b0d32144a71867fbdebb2f77cd20 | actual vendored MIT/public-domain text retained |
| Shader distance functions | src/gfx/gl_batch.cpp plus kit notice | Quilez attribution retained; exact MIT permission/copyright text missing |
| ps5-opengl 1.0.0 | prepare-opengl.sh and native app link group select libPS5OpenGL.a | GPL project plus Mesa/OpenGNM/header licenses; exact patched source archive missing |
| LLVM C++/unwind/builtins | prepare-opengl.sh GROUP explicitly names libc++.a, libc++abi.a, libunwind.a, libclang_rt.builtins-x86_64.a | STATIC link inputs, not header-only. Upstream kit notice saying complete C++ archives are not redistributed does NOT establish absence of linked portions. Preserve LLVM license and audit selected members |
| SDK CRT/libc | app custom startup/tooling and companion stock SDK driver are distinct; SDK libc.a/crt1.o and import stubs are different things | GPL default and actual per-file terms apply; member/source closure and exact /opt pin remain open |
| Generated libc.prx | kit tooling/native/libc_builder.cpp and runtime manifests | independently authored GPL runtime; source in kit snapshot. Separate shim pin alone is not source-to-module proof |
| json-c | companion Makefile links -ljson-c; installed json-c.pc reports 0.19 | upstream COPYING plus installed header attributions retained; exact ports recipe/patches/member notices unresolved |
| zlib | native host converter links cached zlib 1.3.2 | host-only compression license/source preserved; not asserted target code |
| SCE import stubs / console providers | link/conversion metadata, not copied console implementation | do not bundle Sony modules or claim stub inventory is complete static code audit |
| GoogleTest, optional SDL/ImGui/NanoVG/Sokol/CTS and curl/OpenSSL/miniz/Lapy | broad vendor notices mention them; no selection inferred from mention | NOT classified as linked diagnostic dependencies without final receipt |

The retained preparatory native app.ninja and its link/convert command excerpt
are evidence of the earlier app only. They do not attest a final diagnostic
companion or approval of credential preparation. The baseline resident source
list includes the virtual-game backend; diagnostic exclusion must be proven by
the separate final native lane, not by these notice files.
