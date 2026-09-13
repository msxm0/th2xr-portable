#!/usr/bin/env python3
"""Replace MSVC `_asm` blocks with their C fallbacks' empty sibling.

Every `_asm` block in this source sits inside a CPU-feature branch -
    if(SSE2_Flag){ _asm{...} } else if(MMX_Flag){ _asm{...} } else { ...C... }
- so emptying the blocks and forcing the feature flags to zero leaves the
plain-C path, which is what the reference build wants anyway: identical
arithmetic on every host, no SSE rounding to explain away.

Writes patched copies; the GPL tree is never modified.
"""
import pathlib, re, sys

def neutralise(text):
    """Empty every `_asm` block, skipping comments and string literals.

    A naive regex matched `_asm` inside a /* */ comment in MM_std.cpp and ate
    the closing delimiter, so the scan tracks state.
    """
    out, i, n = [], 0, len(text)
    blocks = lines = 0
    while i < n:
        c = text[i]
        # comments and strings are copied through untouched
        if c == '/' and i + 1 < n and text[i+1] == '*':
            j = text.find('*/', i + 2)
            j = n if j == -1 else j + 2
            out.append(text[i:j]); i = j; continue
        if c == '/' and i + 1 < n and text[i+1] == '/':
            j = text.find('\n', i)
            j = n if j == -1 else j
            out.append(text[i:j]); i = j; continue
        if c in '"\'':
            q, j = c, i + 1
            while j < n and text[j] != q:
                j += 2 if text[j] == '\\' else 1
            j = min(j + 1, n)
            out.append(text[i:j]); i = j; continue
        kw = None
        for cand in ('__asm', '_asm'):
            if text.startswith(cand, i) and (i == 0 or not (text[i-1].isalnum() or text[i-1] == '_')):
                kw = cand; break
        if kw:
            after = i + len(kw)
            if after < n and (text[after].isalnum() or text[after] == '_'):
                out.append(c); i += 1; continue
            j = after
            while j < n and text[j] in ' \t\r\n':
                j += 1
            if j < n and text[j] == '{':
                depth, k = 0, j
                while k < n:
                    if text[k] == '{': depth += 1
                    elif text[k] == '}':
                        depth -= 1
                        if depth == 0:
                            k += 1; break
                    k += 1
                out.append('{ /* _asm removed for the reference build */ }')
                i = k; blocks += 1
            else:
                k = text.find('\n', j)
                k = n if k == -1 else k
                out.append(';')
                i = k; lines += 1
            continue
        out.append(c); i += 1
    return ''.join(out), blocks, lines

# Every MSVC-ism that GCC will not take, as an explicit table.  Each entry is
# (file, before, after, why) so the whole divergence from the GPL sources is
# one reviewable list rather than a pile of edits in the tree.
PATCHES = [
    # C89 implicit int.  MSVC accepted it in C++ mode; GCC does not.
    ("ScriptEngine/src/GM_Avg.cpp", "\tstatic\tscnt=0;", "\tstatic\tint scnt=0;", "implicit int"),
    ("ScriptEngine/src/GM_Avg.cpp", "\tstatic\tselect_back=0;", "\tstatic\tint select_back=0;", "implicit int"),
    ("ScriptEngine/src/GM_Avg.cpp", "\tstatic\tselect_chr;", "\tstatic\tint select_chr;", "implicit int"),
    ("ScriptEngine/src/GM_Avg.cpp", "\tstatic\tmleyer;", "\tstatic\tint mleyer;", "implicit int"),
    ("ScriptEngine/src/Winmain.cpp", "static NotInitDirectDraw=0;", "static int NotInitDirectDraw=0;", "implicit int"),
    ("ScriptEngine/src/GM_Demo.cpp", "\tstatic\t\tan=0;", "\tstatic\t\tint an=0;", "implicit int"),
    # MSVC let a for-loop variable outlive its loop (/Zc:forScope-).
    ("my_inc2/readFile.cpp",
     "\tfor(int i=0;i<ArcFile[arcFileNum].fileCount;i++){",
     "\tint i;\n\tfor(i=0;i<ArcFile[arcFileNum].fileCount;i++){",
     "MSVC for-scope leak: i is used after the loop"),
    # --- harness instrumentation (only these three touch behaviour) ---
    # The virtual clock's tick.  MAIN_Loop is the engine's frame, so this is
    # where a frame begins.
    ("ScriptEngine/src/main.cpp",
     "void MAIN_Loop( void )\r\n{\r\n\tstatic int\t\tFrameLimit  = 1000;",
     "void MAIN_Loop( void )\r\n{\r\n#ifdef TH2REF_TRACE\r\n\tth2ref_advance_tick();\r\n#endif\r\n\tstatic int\t\tFrameLimit  = 1000;",
     "virtual clock: advance one tick per MAIN_Loop"),
    # The framebuffer, taken where MAIN_DrawGraph left it and before the
    # DirectDraw blit - no window, no compositor, no scaling.
    ("ScriptEngine/src/main.cpp",
     "\tSendMessage( MainWindow.hwnd, WM_PAINT,0,0);",
     "#ifdef TH2REF_TRACE\r\n\tth2ref_dump_frame( MainWindow.draw_mode2==32 ? (void*)&MainWindow.vram_true :\r\n\t                   MainWindow.draw_mode2==24 ? (void*)&MainWindow.vram_full :\r\n\t                   (void*)&MainWindow.vram_high, MainWindow.draw_mode2 );\r\n#endif\r\n\tSendMessage( MainWindow.hwnd, WM_PAINT,0,0);",
     "framebuffer dump after MAIN_DrawGraph"),
    # A Windows path separator in an #include.
    ("ScriptEngine/src/Escript.cpp", r'#include "..\\mes\\escr.h"', '#include "escr.h"',
     "backslash include path"),
]

GPL = pathlib.Path(sys.argv[1])
GEN = pathlib.Path(sys.argv[2]); GEN.mkdir(parents=True, exist_ok=True)
ASM_FILES = ["my_inc2/MM_std.cpp", "my_inc2/Draw24.cpp", "my_inc2/DrawPrim24.cpp",
             "my_inc2/Draw32.cpp", "my_inc2/DrawPrim32.cpp"]

targets = {}
for rel in ASM_FILES:
    targets.setdefault(rel, [])
for rel, before, after, why in PATCHES:
    targets.setdefault(rel, []).append((before, after, why))

total_b = total_l = total_p = 0
for rel, patches in sorted(targets.items()):
    src = GPL/"ToHeart2"/rel
    raw = src.read_bytes().decode("cp932", "surrogateescape")
    b = l = 0
    if rel in ASM_FILES:
        raw, b, l = neutralise(raw)
    applied = 0
    for before, after, why in patches:
        if before not in raw:
            print(f"  !! {rel}: patch not found ({why})")
            continue
        raw = raw.replace(before, after, 1)
        applied += 1
    (GEN/pathlib.Path(rel).name).write_bytes(raw.encode("cp932", "surrogateescape"))
    note = []
    if b or l: note.append(f"{b} asm blocks, {l} asm lines")
    if applied: note.append(f"{applied} patches")
    print(f"  {pathlib.Path(rel).name:18s} {', '.join(note)}")
    total_b += b; total_l += l; total_p += applied
print(f"total: {total_b} asm blocks, {total_l} asm statements, {total_p} patches")
