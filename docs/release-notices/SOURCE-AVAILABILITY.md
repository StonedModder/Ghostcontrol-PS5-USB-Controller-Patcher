# Corresponding source: diagnostic artifact closure

This supersedes the preparatory missing-final-receipt list. It is a verified
retrieval/attribution supplement for diagnostic source
`dd4c873c384c493b80f8f6cb6400a96736290f58`, not an all-license clearance or
permission to execute a payload. No work is relicensed.

## Owner-only testing versus conveying copies

GPLv3 section 2 permits making/running/propagating covered works that are not
conveyed, without distribution conditions while the license remains in force.
Its contractor/facilities provision is conditional, not a blanket exemption
for anything called a private release.[4]

Running the owner's diagnostic in the owner's existing account and on the
owner's equipment does not by itself require public source publication, a new
global project LICENSE, or a grant from the owner to itself. No such extra
prerequisite is imposed here. This describes the testing boundary, not a
judgment about the parties' legal relationship.

A separate recipient, including a friend receiving a binary from a private
repository, can still receive a conveyed GPL-covered copy. Private visibility
alone does not waive sections 4–6: give recipients matching preferred source,
modifications, build/install scripts and notices through an applicable section
6 mechanism, without restricting GPL rights. Installation Information depends
on section 6's User Product provisions; it is not a universal demand for
console keys or an assumption that owner-only testing is a consumer-product
transaction.[4]

This local preparation is neither a written source offer nor proof of source
delivery. Wider redistribution should carry the notice supplement and source
bundle together. MIT/BSD/Apache/CC conditions remain separate from GPL source
delivery; they do not generally demand every component's entire source tree.

## Bound artifacts

`SOURCE-CLOSURE-RECEIPT.json` binds the native receipts to:

- Companion SHA-256 `10e9e809269def8d142cf94156c30d1e39e367337d5169e81e60ce3f5c85d671`.
- App LLVM PIE ELF `0d1a72cea5f148041790eaae40faae846a6b2e0d1de590643de71bcc57cc56c9`.
- Title ZIP `ebc1c07d22539a2a8efc8db73c3532887b2e95255a655e540090da2a27486962`.
- Project source archive `d00ae22d8386e36b3d08c37a684d4d1af4b0035b1a18d078af86316e7065f037`.

All 1,287 tracked project blobs match that source archive. The existing ZIP's
47 notice inputs were independently byte-checked. This docs-only task does not
rebuild/repack/replace that ZIP or claim it already contains the new notices.
Supplemental notices and source evidence are separately prepared on G:.

## Retrieved identity/source closure

| Input | Established result |
|---|---|
| Project | Exact diagnostic source above replaces the earlier default-resident baseline as the matching source reference. |
| UI kit / boilerplate / shim | Prior complete tracked snapshots retained at `4bd942579dd981b3df9c740438489ca6a614ddc0`, `3ada439fa044f60c8b488678577a740761385711`, `722f2227a8bb6fa2229120546995b6562552c752`. Generated libc.prx source is kit tooling/native, not inferred solely from the separate shim. |
| OpenGL SDK 1.0.0 | Official archive SHA-256 `f93643c04c843d56143b00951df1f8042ea7706ae9f19e4e9abf158e1ead77c5`; every cached SDK regular file compared byte-identically. All ten source companions match provenance hashes. Runtime/source pin `dce74910af7f4274c48d05e0ae001b3038462588`. |
| Mesa / PSBC / headers | Published source companions, pristine pinned archives, PS5 patches, generators and build scripts now present. Mesa archive hash and PSBC patch hash match OPENGL-SOURCE-PINS.json. Sources plus patches are retained, not substituted by license texts or claimed rebuilt. |
| App SDK | Source `4eb701204fc3f8d31e84cf8ca272974e2be9c867` (v0.42) retrieved. Installed app libc++, libc++abi, libunwind and libc archives byte-match the official cached v0.42 ZIP and its published hash. |
| Companion SDK | `/opt` CRT and libc byte-match official v0.43 ZIP, hash `a9cc9929f21b2b2c5d5b309f3bab4997067c45281c0622cf4838b1aecba66fcb`. Matching source `d9c9519116944a7f1c22d262012c53b80b3520b7` retrieved. NOT guessed v0.42 from the app toolchain. |
| json-c / libm | Installed archives match official cached PacBrew v0.40.3 image hash `0d71dd90c4ad6ef562923be0eb87ca8ce47825b6a2f209e04bb5d57d3359aee8`. Recipes at `1687e827675c66e1b040751c7f0811222bd3aea0` retained. json-c 0.19 source release hash `37ad0249902e301bd9052bf712e511fcc6acff4ecaad4b5900aad9ce564e26de`; openlibm 0.8.6 source hash `347998968cfeb2f9b91de6a8e85d2ba92dec0915d53500a4bc483e056f85b94c`. Neither recipe adds source patches; implementation notices retained. |
| LLVM | SDK runtime recipe specifies 18.1.8; complete upstream tag source retrieved. System emutls.c.o is instead from installed libclang-rt-18-dev, source llvm-toolchain-18 `1:18.1.3-1ubuntu1`; installed copyright, Apache/LLVM terms and upstream 18.1.3 emutls source retained. Do not label it 18.1.8. |

The OpenGL release explicitly includes sources/checksums/provenance.[1] Exact
PacBrew v0.40.3 CI confirms SDK `0.43.r0.gd9c9519`, json-c 0.19 and openlibm
0.8.6. Its SDK recipe uses an otherwise unpinned Git source: the ports tag name
alone would not identify the SDK.[2][8]

`SOURCE-ARCHIVES.json` records hashes, sizes and member safety. Large files are
under `/mnt/g/Ghostcontrol/pc-ready-20261015/source-closure/`; prior snapshots
remain in `notices-source-preparation/`. No dependency was installed or built;
no downloaded program was executed. Relative symlinks were normalized inside
the archive root; benign `..` links are not escape findings. No archive was
blindly extracted.

The independent import audit additionally matches the same companion CRT in
official v0.43 and v0.44 archives: the object hash alone is not a unique
version tag. The PacBrew v0.40.3 CI receipt identifies its build as v0.43.
The retrieved v0.43 crt/crt.c adds the payloadout null guards missing from
v0.42, resolving the audit’s concrete source discrepancy. The retained diff
is SDK-CRT-v042-v043.diff. This is upstream release/source provenance and
semantic corroboration, not a reproducible-build or unique-version proof.
No generic v0.42 CRT source is labelled matching companion source.

## Member-map generality

Native byte-exact relink receipts identify 26 unique companion archive members
and 757 unique app archive members, including 336 PSBC, 16 libc++, 14 libc++abi,
four libunwind and one system emutls object. Dynamic imports alone would miss
these. Full maps/receipts are retained in the evidence bundle.

SOURCE-MEMBER-CANDIDATES.json is a filename-based lookup: 757 of 783 combined
entries have source candidates; 26 require generator/special-source tracing.
It is not source/object reproduction or proof that every ambiguous candidate
linked. The full sources and build/generator scripts, not this heuristic,
constitute the source preparation. Generated C/C++ does not necessarily replace
a generator as preferred source. Build replay was not performed.

Candidate implementation copyright/license headers are preserved in additional
notices. Complete sources retain original per-file headers and generator
notices missed by the lookup. Component-level license files do not override
per-file terms. Original notice texts remain intact.

## Exact remaining questions

1. **Shader grant unauthenticated.** The Quilez article is now retrievable and
   contains the functions and the author's derivation statement, but no MIT
   copyright/permission text was found. The pinned kit's MIT claim remains its
   claim, not an authenticated author grant. Shadertoy returned 403; the old
   alternate URL returned 404; public code/history searches did not establish
   author-issued MIT text. Before onward distribution obtain applicable
   permission from author/upstream or independently replace the adaptations.
   No copyright year, MIT text or grant is invented.[5]
2. **Model attribution identified; exact edit input missing.** Existing
   reference/view/MODEL-ATTRIBUTION.md, public model API and retrieved GLB
   metadata agree on Taohid Animation / PS5 Controller / CC BY 4.0. Credit and
   full license are now supplemental notices. The native converter describes
   an 81-part studio input; the mirror has 48 mesh entries, so it is not
   asserted to be the exact conversion input. CC BY requires attribution, not
   Blender-source delivery; whether a missing edit input forms preferred source
   of a GPL-covered adaptation is a separate question.[6][7]
3. **Owner-supplied replacements are not kit assets by assumption.** Project
   logo and ui/sce_sys icon0.png, pic0.dds, pic1.dds, snd0.at9 enter the owner's
   initial commit; sce_sys bytes differ from the kit. Git does not identify a
   third-party author, prove original ownership, or identify preferred editable
   inputs. Treat these as owner-supplied assets pending origin/edit-input
   confirmation—not proven unlicensed works and not a requirement for a global
   LICENSE before owner testing. Preserve identified third-party terms and
   preferred source where applicable on onward delivery. ASSET-PROVENANCE.json
   retains exact history and hashes.
4. **Delivery not performed.** Supplements exist locally; no GitHub write or
   recipient-access readback occurred. Conveying copies requires selecting a
   delivery mechanism and verifying access. Owner-only/nonconveying activity
   does not become a public publication duty because this bundle exists.

These are remaining attribution/source questions, not an all-clear or an
indiscriminate block on owner testing. Hardware readiness, import approval,
credential/source authority and console execution remain separate.

## Sources

[1] https://api.github.com/repos/blackbearreloaded/ps5-opengl/releases/tags/v1.0.0
[2] https://github.com/ps5-payload-dev/pacbrew-repo/actions/runs/33272249753
[4] https://raw.githubusercontent.com/ps5-payload-dev/sdk/4eb701204fc3f8d31e84cf8ca272974e2be9c867/LICENSE
[5] https://iquilezles.org/articles/distfunctions2d
[6] https://api.sketchfab.com/v3/models/b7bb9c5102a04cb0b1966c6d02bad7d6
[7] https://creativecommons.org/licenses/by/4.0/legalcode.txt
[8] https://raw.githubusercontent.com/ps5-payload-dev/pacbrew-repo/1687e827675c66e1b040751c7f0811222bd3aea0/json-c/PKGBUILD
