# Platform adapter provenance

The recovered engine is from dcostap/aoe1-decomp at commit
`6d6d8b14023aa74e6ac30a6c88229d5b818fb805`. Its upstream license is retained at
`../upstream/LICENSE`. The port changes are recorded in `../port.patch` and
`../port-changes.json`. This is recovered beta source, not proof of equivalence
to the user's different retail executable.

`ddraw.h` and `dsound.h` are unchanged MinGW-w64 copies of Wine interface
headers under LGPL-2.1-or-later. Their copyright/license notices are preserved;
the complete LGPL text is in `licenses/LGPL-2.1.txt`. `_mingw_dxhelper.h` is an
unchanged public-domain MinGW-w64 header. Pinned URLs and SHA-256 hashes are in
`source.json`; MinGW notices are retained in `licenses/MinGW-*.txt`.

`win_constants.h` and `win_functions.h` select ABI constants and declarations
from the MinGW-w64 public headers. The remaining small headers adapt the
Win32/MSVCRT types used by this source to wasm32. These declarations do not
implement DirectDraw, DirectSound, GDI, windows, or network transports.

`../upstream/third_party/directplay/include/` retains the original Microsoft
SDK header notices distributed with the reference source. Those third-party
headers are not relicensed by the upstream CC0 license.

The local game assets copied to `../../web/game/` are user-supplied proprietary
game data. No game EXE, PDB, DLL machine code or extracted original object code
is linked into the WASM artifact. No game files have been published externally.

`software_draw.cpp` is the portable indexed implementation of the recovered
`Asm_draw.asm` and selected `Drawarea_asm` operations. `draw_mask_lanes.h`
records generic byte-lane indices (0..3) measured by executing synthetic
fixtures against the supplied retail executable; it contains no executable
code or game graphics. The oracle generator and executable hash are recorded
in `../../tests/test_drawing_oracle.py` and the drawing test report. These
component comparisons do not establish full retail equivalence.
