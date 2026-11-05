# Diagnostic release notice preparation

All files in this directory are regular, flat and uniquely named because
companion-manifest.py generate copies --notice inputs by basename.
Pass every file listed in NOTICE-INPUTS.txt as a separate --notice argument;
for ui/Makefile pass the same space-separated absolute paths as
GHOSTTOOTH_NOTICES. No path in this bundle contains whitespace.

This set covers both app and companion obligations: fonts and audio shipped
from the kit, embedded shader/decoder code, static OpenGL/Mesa/OpenGNM/compiler
inputs, LLVM runtime/header notices, SDK GPL/default and retained installed
FreeBSD header notices, and json-c. Kit notices also preserve authorship of
its generated libc.prx. Host-only zlib/tooling and optional example licenses
are explicitly context, not assertions of target linkage. GoogleTest, optional
SDL/ImGui/NanoVG/Sokol/CTS, Lapy, curl/OpenSSL and miniz are not declared linked
merely because a broad upstream notice mentions them. Compare the final link
and package inventory before adding/removing applicability.

SOURCE-AVAILABILITY.md supersedes the earlier missing-final-receipt list with
verified retrieval closure and exact remaining questions. It separates
owner-only testing from conveying copies; it is not comprehensive legal
clearance or permission to execute a payload. NOTICE-INVENTORY.json records
copied-file hashes and origin; FETCH-PROVENANCE.json includes failed requests
as well as the successful small upstream license/metadata fetches.
No OpenProspero runtime or release path is selected.

The existing diagnostic ZIP retains its original 47 notice inputs. Additional
implementation notices, the independently authenticated controller credit and
CC BY text, system LLVM 18.1.3 notices and current source receipts form a
separate supplement; do not claim they were already in that ZIP. Full matching
source archives preserve original header/generator notices and should accompany
the supplement on onward delivery. No third-party source is relicensed.
