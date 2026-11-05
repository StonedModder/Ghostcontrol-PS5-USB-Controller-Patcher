# Third-party notices

## Credits and acknowledgements

| Project | Role |
| --- | --- |
| [Mesa](https://www.mesa3d.org/) | OpenGL, Gallium, GLSL/NIR, ACO/RADV and AMD layout infrastructure |
| [OpenGNM PSBC](https://github.com/PS4-OpenGNM/opengnm-psbc) / [OpenGNM](https://github.com/PS4-OpenGNM/opengnm) | Shader compiler foundation and reference declarations |
| [PS5 Payload SDK](https://github.com/ps5-payload-dev/sdk) | Public homebrew toolchain and imports |
| [Native app boilerplate](https://github.com/blackbearreloaded/ps5-native-app-boilerplate) | Native app assembly and folder packaging |
| [PS5 GPU research](https://github.com/blackbearreloaded/ps5-gpu-research) | Shader toolchain, memory, submission and presentation findings |
| [Khronos VK-GL-CTS](https://github.com/KhronosGroup/VK-GL-CTS) | Pinned OpenGL test inventory and runner |
| [SDL2](https://github.com/libsdl-org/SDL), [Dear ImGui](https://github.com/ocornut/imgui), [NanoVG](https://github.com/memononen/nanovg), [Sokol](https://github.com/floooh/sokol) | Integration and renderer examples |

Project-owned code uses the repository's [GPL-3.0-or-later license](LICENSE). This does not
relicense upstream components. Preserve their notices and applicable per-file
licenses in derived sources and redistributed builds.

BlackBearReloaded's project headers identify project-owned files. Contribution
notices on upstream patches and the generated Sokol adaptation credit only the
PS5 changes, not upstream authorship. Those changes remain subject to the
applicable upstream per-file licenses; generated Khronos declarations and
unmodified dependencies are not claimed as project-owned code.

Core/example sources are fetched at immutable revisions from [dependencies.json](dependencies.json).
The optional SDL2 integration separately pins and exports its supplied source checkout.
No complete upstream source checkout, public payload SDK, vendor SDK or firmware
module is copied into this repository. Patches and the test inventory are included.

| Project | Use / modifications | License reference |
| --- | --- | --- |
| [Mesa 26.2.0](https://www.mesa3d.org/) | GLSL, NIR, Gallium, GL dispatch/state tracker, ACO/RADV, AMD AddressLib and utilities; PS5 patch included | Primarily MIT; component/file-specific licenses in [LICENSES/Mesa](LICENSES/Mesa) and fetched source |
| [OpenGNM PSBC](https://github.com/PS4-OpenGNM/opengnm-psbc) | Shader compiler; complete PS5 delta over public `a92a1228` included | [MIT](LICENSES/OpenGNM-PSBC.txt); retained Mesa file notices also apply |
| [OpenGNM](https://github.com/PS4-OpenGNM/opengnm) | Shader/container reference declarations | [MIT](LICENSES/OpenGNM.txt) |
| [SPIRV-Headers](https://github.com/KhronosGroup/SPIRV-Headers) | Compiler grammar/header inputs | Fetched `LICENSE` and per-file notices |
| [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) | Declarations required by the compiler's RADV subset; not a Vulkan port | Fetched `LICENSE.md`; Apache-2.0/MIT by file |
| [VK-GL-CTS](https://github.com/KhronosGroup/VK-GL-CTS) | Inventory, runner and six disclosed platform/test adaptations | [Apache-2.0](LICENSES/VK-GL-CTS.txt) and component notices |
| [Dear ImGui 1.91.9b](https://github.com/ocornut/imgui) | Unmodified renderer/core in the examples | [MIT](LICENSES/Dear-ImGui.txt) |
| [NanoVG](https://github.com/memononen/nanovg) | Unmodified GL3 renderer | [zlib](LICENSES/NanoVG.txt); preserve embedded dependency notices |
| [Sokol](https://github.com/floooh/sokol) | Unmodified GL backend; selects existing 3.3 fallback | [zlib/libpng](LICENSES/Sokol.txt) |
| [Sokol samples](https://github.com/floooh/sokol-samples) | Cube sample with disclosed native platform/GLSL-version adaptations | [MIT](LICENSES/Sokol-Samples.txt) |
| [vecmath](https://github.com/floooh/sokol-samples/tree/8afa83928ce1870efeb0d513e7c4dce4f5db7b3e/libs/vecmath) | Unmodified matrix/vector header bundled with the pinned cube sample | [MIT option](LICENSES/vecmath.txt) |
| [PS5 SDL2 fork](https://github.com/ps5-payload-dev/SDL) / [SDL](https://github.com/libsdl-org/SDL) | Optional SDL 2.30.12 video integration at `8c56053f13ca13a0c050de613706ff69eb615836`; retains upstream core/events/joystick, replaces the selected video backend and marks the static-dynapi alteration | [zlib](LICENSES/SDL2.txt); upstream per-file notices retained in exported source |
| [PS5 Payload SDK](https://github.com/ps5-payload-dev/sdk) | Public homebrew compilers, import libraries and C/C++ support; build prerequisite | GPL-3.0-or-later, Copyright (C) John Törnblom, plus the SDK's component/per-file licenses; not a vendor SDK |
| [Native app boilerplate](https://github.com/blackbearreloaded/ps5-native-app-boilerplate) | Native runtime, linker/container conversion, assets and folder assembly | Its GPL-3.0-or-later and retained component notices |
| [LLVM](https://llvm.org/) | Clang/LLD, compiler builtins, libc++, libc++abi and libunwind | Apache-2.0 with LLVM exceptions or component-specific notices |
| [zlib](https://zlib.net/) | Native boilerplate build-time compression | zlib license in fetched source |

The compiler-only linked-tessellation helpers in the PSBC patch adapt
`src/amd/vulkan/radv_pipeline_graphics.c` from the pinned Mesa/RADV source:
Copyright © 2016 Red Hat; Copyright © 2016 Bas Nieuwenhuizen; based in part on
the ANV driver, Copyright © 2015 Intel Corporation. These portions retain their
MIT license, not the project's license. The host regressions also extract
unchanged Mesa functions from the fetched sources; their upstream notices apply.

The build also uses Python, GNU Make/binutils, Meson, Ninja, Mako, PyYAML,
packaging, glslang and SPIR-V Tools. These are host tools, not bundled runtime
implementations; their own projects retain their licenses.

The minimal TV-demo pad declarations were adapted from the independently authored
`ps5-input-investigation` research header; no vendor header is included. Related
research references are in the main README. Historical test descriptions and
upstream names do not imply endorsement or formal conformance.

SDK archives retain runtime/dependency notices and corresponding source archives,
including the project patches and build files. The public payload SDK and native
boilerplate remain separate prerequisites, not bundled binaries. Before
redistributing SDKs or applications, review the applicable component and
source-distribution requirements; generated application bundles can have additional
dependencies beyond this static graphics SDK.
