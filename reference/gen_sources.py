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
    # --- audio as events.  Nothing is decoded on this side, so what can
    # be compared is which sound was asked for, on which channel, when.
    ("ScriptEngine/src/GM_Avg.cpp",
     'void AVG_PlaySE2( int sno, int se_no, int fade, int loop, int vol )\r\n{',
     'void AVG_PlaySE2( int sno, int se_no, int fade, int loop, int vol )\r\n{\r\n#ifdef TH2REF_TRACE\r\n\tth2ref_note_audio("se2", sno, se_no, loop, vol, 0);\r\n#endif',
     "audio: SE on a channel"),
    ("ScriptEngine/src/GM_Avg.cpp",
     'int AVG_PlaySE( int se_no, int loop, int vol, int direct_vol )\r\n{',
     'int AVG_PlaySE( int se_no, int loop, int vol, int direct_vol )\r\n{\r\n#ifdef TH2REF_TRACE\r\n\tth2ref_note_audio("se", -1, se_no, loop, vol, 0);\r\n#endif',
     "audio: transient SE"),
    ("ScriptEngine/src/GM_Avg.cpp",
     'void AVG_StopSE2( int sno, int fade )\r\n{',
     'void AVG_StopSE2( int sno, int fade )\r\n{\r\n#ifdef TH2REF_TRACE\r\n\tth2ref_note_audio("sestop", sno, -1, fade, 0, 0);\r\n#endif',
     "audio: SE stop"),
    ("ScriptEngine/src/GM_Avg.cpp",
     'void AVG_PlayVoice( int vc_no, int cno, int sno, int vno, int vol, int loop, int a_cut, int test )\r\n{',
     'void AVG_PlayVoice( int vc_no, int cno, int sno, int vno, int vol, int loop, int a_cut, int test )\r\n{\r\n#ifdef TH2REF_TRACE\r\n\tth2ref_note_audio("voice", vc_no, cno, sno, vno, 0);\r\n#endif',
     "audio: voice"),
    ("ScriptEngine/src/GM_Avg.cpp",
     'void AVG_PlayBGM( int mus_no, int fade, int loop, int vol, int change )\r\n{',
     'void AVG_PlayBGM( int mus_no, int fade, int loop, int vol, int change )\r\n{\r\n#ifdef TH2REF_TRACE\r\n\tth2ref_note_audio("bgm", -1, mus_no, loop, vol, 0);\r\n#endif',
     "audio: BGM"),
    ("ScriptEngine/src/GM_Avg.cpp",
     'int AVG_StopBGM( int fade )\r\n{',
     'int AVG_StopBGM( int fade )\r\n{\r\n#ifdef TH2REF_TRACE\r\n\tth2ref_note_audio("bgmstop", -1, -1, fade, 0, 0);\r\n#endif',
     "audio: BGM stop"),
    ("my_inc2/soundDS.cpp",
     'ClResult ClSoundDS::playVoice(int &handle,int arcFileNum,char *fname, int repeat, int vol )\r\n{',
     'ClResult ClSoundDS::playVoice(int &handle,int arcFileNum,char *fname, int repeat, int vol )\r\n{\r\n#ifdef TH2REF_TRACE\r\n\tth2ref_note_audio("voicefile", arcFileNum, repeat, vol, 0, fname);\r\n#endif',
     "audio: resolved voice filename"),
    ("ScriptEngine/src/GM_Avg.cpp",
     'void AVG_PlaySE3( int se_no, int vol )\r\n{',
     'void AVG_PlaySE3( int se_no, int vol )\r\n{\r\n#ifdef TH2REF_TRACE\r\n\tth2ref_note_audio("se", -1, se_no, 0, vol, 0);\r\n#endif',
     "audio: SE opcode (ESC_EOprSE goes via PlaySE3)"),
    ("ScriptEngine/src/GM_Avg.cpp",
     'int AVG_PlaySePan( int se_no, int loop, int vol, int lr )\r\n{',
     'int AVG_PlaySePan( int se_no, int loop, int vol, int lr )\r\n{\r\n#ifdef TH2REF_TRACE\r\n\tth2ref_note_audio("se", -1, se_no, loop, vol, 0);\r\n#endif',
     "audio: panned SE"),
    ("ScriptEngine/src/Escript.cpp",
     'static void ESC_EOprVS( void )\r\n{\r\n\tif(EscParam[0].num==-1) EscParam[0].num = 0;\t\t\r\n\tif(EscParam[1].num==-1) EscParam[1].num = 0;\t\t\r\n\t',
     'static void ESC_EOprVS( void )\r\n{\r\n\tif(EscParam[0].num==-1) EscParam[0].num = 0;\t\t\r\n\tif(EscParam[1].num==-1) EscParam[1].num = 0;\t\t\r\n\t#ifdef TH2REF_TRACE\r\n\tth2ref_note_audio("voicestop", EscParam[1].num, -1, -1, -1, 0);\r\n#endif\r\n\t',
     "audio: the script's own voice stop, not the housekeeping one"),
    ("ScriptEngine/src/GM_AvgMsg.cpp",
     '\t\tfor(i=0;i<10;i++)\r\n\t\t\tDSP_SetGraphDisp( GRP_WINDOW+i, ON );\r\n',
     '\t\tfor(i=0;i<10;i++)\r\n\t\t\tDSP_SetGraphDisp( GRP_WINDOW+i, ON );\r\n#ifdef TH2REF_TRACE\r\n\t\tif(th2ref_text_hidden())\r\n\t\t\tfor(i=0;i<10;i++) DSP_SetGraphDisp( GRP_WINDOW+i, OFF );\r\n#endif\r\n',
     "the message window frame, which TH2REF_NOTEXT did not cover"),
    # AVG_GetHitKey probe.  Reads the same two terms the function returns,
    # without changing what it returns.
    ("ScriptEngine/src/GM_Avg.cpp",
     "BOOL AVG_GetHitKey( void )\r\n{\r\n\treturn GameKey.click && (MUS_GetMouseNo(-1)==-1);\r\n}",
     "BOOL AVG_GetHitKey( void )\r\n{\r\n#ifdef TH2REF_TRACE\r\n\tth2ref_note_hitkey( GameKey.click, MUS_GetMouseNo(-1) );\r\n#endif\r\n\treturn GameKey.click && (MUS_GetMouseNo(-1)==-1);\r\n}",
     "hit-key probe: record the mouse rect on clicking frames"),
    # The framebuffer, taken where MAIN_DrawGraph left it and before the
    # DirectDraw blit - no window, no compositor, no scaling.
    ("ScriptEngine/src/main.cpp",
     "\tSendMessage( MainWindow.hwnd, WM_PAINT,0,0);",
     "#ifdef TH2REF_TRACE\r\n\tth2ref_dump_frame( MainWindow.draw_mode2==32 ? (void*)&MainWindow.vram_true :\r\n\t                   MainWindow.draw_mode2==24 ? (void*)&MainWindow.vram_full :\r\n\t                   (void*)&MainWindow.vram_high, MainWindow.draw_mode2 );\r\n#endif\r\n\tSendMessage( MainWindow.hwnd, WM_PAINT,0,0);",
     "framebuffer dump after MAIN_DrawGraph"),
    # Scripted input.  Both Renew calls read the real devices; the mouse is
    # the subtle one, because the engine hit-tests the system bar against the
    # live cursor, so where the pointer happens to be would change the path
    # a trace run takes.
    ("ScriptEngine/src/main.cpp",
     "\tKEY_RenewKeybord( MainWindow.active );\r\n\tMUS_RenewMouse( MainWindow.hwnd, MainWindow.active, 0 );",
     "#ifdef TH2REF_TRACE\r\n\tth2ref_input();\r\n\tMUS_RenewMouse( MainWindow.hwnd, MainWindow.active, 0 );\r\n#else\r\n\tKEY_RenewKeybord( MainWindow.active );\r\n\tMUS_RenewMouse( MainWindow.hwnd, MainWindow.active, 0 );\r\n#endif",
     "scripted input replaces the real devices"),
    # The state trace, at the end of the tick body rather than next to the
    # frame dump: MAIN_DrawControl sits behind skip_cnt and can be skipped,
    # and a state line that goes missing exactly when the engine is under
    # load is the opposite of what the trace is for.
    ("ScriptEngine/src/main.cpp",
     "\t\t\tMainStep = NextMainStep;\r\n\t\t}else{",
     "#ifdef TH2REF_TRACE\r\n\t\t\tth2ref_dump_state();\r\n#endif\r\n\t\t\tMainStep = NextMainStep;\r\n\t\t}else{",
     "state trace at the end of each tick"),
    # Checkpointing, so a window at tick 6000 does not cost six thousand
    # ticks of replay to reach.  SAV_Save is nearly what is wanted and not
    # quite: SAV_CreateSaveHead captures the thumbnail by parking BMP_CAP in
    # GRP_WORK, zooming it to 80x60 and then DSP_ResetGraph-ing the slot -
    # which throws away whatever GRP_WORK held, and the shake cases park a
    # black plate there.  A checkpoint that perturbs the run it is taken
    # from is worse than no checkpoint, so these two are SAV_Save and
    # SAV_Load with the header left out and the filename passed in.  The
    # header bytes in the file are then uninitialised and never read back.
    ("ScriptEngine/src/GM_Save.cpp",
     "\t\t\tAVG_SetLoadData( SaveStruct.sdata );\r\n\tAVG_LoadWindow();\r\n}",
     "\t\t\tAVG_SetLoadData( SaveStruct.sdata );\r\n\tAVG_LoadWindow();\r\n}"
     "\r\n\r\n#ifdef TH2REF_TRACE\r\n"
     "void SAV_TraceSave( const char *fname )\r\n{\r\n"
     "\tSAV_SaveScript();\r\n"
     "\tCopyMemory( SaveStruct.ESC_FlagBuf, ESC_FlagBuf, sizeof(int)*ESC_FLAG_MAX );\r\n"
     "\tAVG_SetSaveData( &SaveStruct.sdata );\r\n"
     "\tSTD_WriteFile( (char*)fname, (char*)&SaveStruct, sizeof(SaveStruct) );\r\n"
     "}\r\n\r\n"
     "int SAV_TraceLoad( const char *fname )\r\n{\r\n"
     "\tif( !STD_ReadFile( (char*)fname, (char*)&SaveStruct, sizeof(SaveStruct)) ) return 0;\r\n"
     "\tSAV_LoadScript();\r\n"
     "\tCopyMemory( ESC_FlagBuf, SaveStruct.ESC_FlagBuf, sizeof(int)*ESC_FLAG_MAX );\r\n"
     "\tDefaultCharName = ESC_GetFlag( _DEFAULT_NAME );\r\n"
     "\tAVG_SetLoadData( SaveStruct.sdata );\r\n"
     "\tAVG_LoadWindow();\r\n"
     "\treturn 1;\r\n"
     "}\r\n#endif\r\n",
     "header-free save/load for harness checkpoints"),
    # EOprFlag, reachable from the shim.
    #
    # It is `static char EOprFlag[ESC_OPR_MAX]` inside Escript.cpp, and it is
    # the latch that stops a waiting opcode running its set-up twice:
    #     if(!EOprFlag[ESC_SETMESSAGE2]){ EOprFlag[..]=1; ...set up... }
    # A checkpoint that does not carry it resumes *into* the instruction it
    # was parked on and sets the message up again, throwing away one 205
    # characters into 213.  The engine's own save has no reason to carry it -
    # a player-facing save is taken between messages - so it is exported here
    # for the harness rather than added to AVG_SAVE_DATA.
    ("ScriptEngine/src/Escript.cpp",
     "\t\tEOprFlag[i] = 0;\r\n}",
     "\t\tEOprFlag[i] = 0;\r\n}\r\n"
     "\r\n#ifdef TH2REF_TRACE\r\n"
     "extern \"C\" int th2ref_eopr_size( void ){ return ESC_OPR_MAX; }\r\n"
     "extern \"C\" void *th2ref_eopr_data( void ){ return (void*)EOprFlag; }\r\n"
     "#endif\r\n",
     "export EOprFlag for checkpoints"),
    # Text off for trace runs.  The reference composites its text into the
    # same buffer as the art; ours keeps it on a separate monitor-resolution
    # layer, so the two could never match pixel for pixel and a diff of them
    # would be dominated by a difference we chose on purpose.  What the
    # typewriter is doing is carried by NovelMessage.count in the state
    # trace, which is the part that actually goes wrong.
    #
    # Through TXT_DrawTextEx's cnt_flag rather than by returning early.
    # Every FNT_Draw call inside it is guarded by `if(!cnt_flag)`, but *px2
    # and *py2 - the text cursor - are written either way, and the click
    # indicator is positioned from them.  Skipping the call outright left
    # the cursor at its initial value and parked GRP_KEYWAIT in the top left
    # corner, which then showed up in the pixel diff as a divergence that
    # was purely an artifact of the instrument.
    # Which text slot is being drawn.  The per-glyph recorder needs it: a
    # frame draws several text objects and only TXT_WINDOW is the message.
    # Picking "the longest one this tick" instead looked like it worked and
    # quietly mixed three different objects into one column.
    ("my_inc2/DISP.CPP",
     "BOOL DrawGraphText( void *dest, int x, int y, TEXT_STRUCT *ts, int draw_mode, int kaigyou_musi )\r\n{\r\n",
     "BOOL DrawGraphText( void *dest, int x, int y, TEXT_STRUCT *ts, int draw_mode, int kaigyou_musi )\r\n{\r\n"
     "#ifdef TH2REF_TRACE\r\n\tth2ref_text_slot((int)(ts - TextStruct));\r\n#endif\r\n",
     "per-glyph alpha: which text slot"),
    ("my_inc2/DISP.CPP",
     "\tif(ts->brt_flag)TXT_DrawText(",
     "#ifdef TH2REF_TRACE\r\n"
     "\tif(th2ref_text_hidden()){\r\n"
     "\t\tTXT_DrawTextEx( dest, draw_mode, ts->dx+x, ts->dy+y, ts->ws, ts->hs,"
     " ts->pw, ts->ph, &ts->px, &ts->py, &clip, ts->font, ts->str, ts->color,"
     " ts->cnt, ts->step, 128, 128, 128, ts->alph, ts->kage, 1, NULL,"
     " kaigyou_musi );\r\n"
     "\t\treturn TRUE;\r\n"
     "\t}\r\n"
     "#endif\r\n"
     "\tif(ts->brt_flag)TXT_DrawText(",
     "text off in trace runs, by counting rather than skipping"),
    # The click indicator goes with the text.  It is a graph rather than a
    # glyph, so TH2REF_NOTEXT does not touch it, but it is positioned from
    # the text cursor and our port draws it on the same high-resolution layer
    # as the text - so like the text itself it can never match pixel for
    # pixel.  Its display is turned off; everything the engine computed about
    # it - which sprite, which frame of the animation, where it goes - still
    # happens, so the instrument removes pixels rather than behaviour.
    ("ScriptEngine/src/GM_AvgMsg.cpp",
     "\t\t\t\t\tDSP_SetGraphMove( GRP_KEYWAIT, px-2, py-2 );\r\n",
     "\t\t\t\t\tDSP_SetGraphMove( GRP_KEYWAIT, px-2, py-2 );\r\n"
     "#ifdef TH2REF_TRACE\r\n"
     "\t\t\t\t\tif(th2ref_text_hidden()) DSP_SetGraphDisp( GRP_KEYWAIT, OFF );\r\n"
     "#endif\r\n",
     "click indicator off in trace runs"),
    # A sound's length, in ticks instead of on a wall clock.
    #
    # ClSoundDS::GetStatus asks DirectSound whether the buffer has drained,
    # and the engine's SE, voice and BGM waits are all gated on the answer.
    # Under the virtual clock that answer arrives after a real second or two
    # no matter how many ticks have gone by, so the same wait costs a
    # different number of ticks on a fast machine than on a slow one.  These
    # three put the answer on the tick counter: play notes when a handle
    # started, Stop clears it, GetStatus reports from it.
    # After the existing guard, not before it.  Reporting PCM_PLAY for a
    # handle whose buffer was never created sent GetPlayStreamVolume into
    # GetCurrentPosition and Lock on a buffer that is not there, and the
    # reference died six ticks into the run.  The virtual answer replaces
    # only the "is it still playing" question, never the "does it exist" one.
    ("my_inc2/soundDS.cpp",
     "\tif(NULL==lpSoundBuffer || PCM_STOP==lpSoundBuffer->status){\r\n"
     "\t\treturn PCM_STOP;\t\t\r\n\t}\r\n"
     "\tlpSoundBuffer->lpDSBuffer->GetStatus(&status);",
     "\tif(NULL==lpSoundBuffer || PCM_STOP==lpSoundBuffer->status){\r\n"
     "\t\treturn PCM_STOP;\t\t\r\n\t}\r\n"
     "#ifdef TH2REF_TRACE\r\n"
     "\tif(th2ref_pcm_status(handle)) return PCM_PLAY;\r\n"
     "\tlpSoundBuffer->status = PCM_STOP;\r\n"
     "\treturn PCM_STOP;\r\n"
     "#endif\r\n"
     "\tlpSoundBuffer->lpDSBuffer->GetStatus(&status);",
     "sound status from the tick counter"),
    ("my_inc2/soundDS.cpp",
     "ClResult ClSoundDS::play(int handle,int repeat)\r\n{\r\n",
     "ClResult ClSoundDS::play(int handle,int repeat)\r\n{\r\n"
     "#ifdef TH2REF_TRACE\r\n\tth2ref_pcm_play(handle,repeat);\r\n#endif\r\n",
     "sound start tick"),
    # Stop is the other half, and it has to be virtual too.  Its tail is
    #
    #     while(lpSoundBuffer->hEvent[0]) Sleep(1);
    #
    # which waits for the streaming thread to acknowledge - and that thread
    # is parked in WaitForMultipleObjects(INFINITE) on events a stubbed
    # decoder never signals.  While sounds finished instantly nothing ever
    # called Stop, so the deadlock was unreachable; the moment they report
    # PCM_PLAY the engine starts stopping them and the reference hung six
    # ticks in.  Under trace it marks the buffer stopped and returns, which
    # is all the bookkeeping a sound that was never really playing needs.
    ("my_inc2/soundDS.cpp",
     "void ClSoundDS::Stop(int handle)\r\n{\r\n"
     "\tClSoundBuffer\t*lpSoundBuffer;\r\n\r\n"
     "\tlpSoundBuffer = SelectSoundBuffer(handle);\r\n",
     "void ClSoundDS::Stop(int handle)\r\n{\r\n"
     "\tClSoundBuffer\t*lpSoundBuffer;\r\n\r\n"
     "\tlpSoundBuffer = SelectSoundBuffer(handle);\r\n"
     "#ifdef TH2REF_TRACE\r\n"
     "\tth2ref_pcm_stop(handle);\r\n"
     "\tif(lpSoundBuffer) lpSoundBuffer->status = PCM_STOP;\r\n"
     "\treturn;\r\n"
     "#endif\r\n",
     "sound stop is bookkeeping only under trace"),
    # The per-glyph alpha vector.  begin() once per TXT_DrawTextEx, glyph()
    # per character - and deliberately outside the `if(!cnt_flag)` guards, so
    # a trace run records the typewriter even though it draws none of it.
    ("my_inc2/text.cpp",
     "\tint\t\talph2=alph;\r\n",
     "\tint\t\talph2=alph;\r\n"
     "#ifdef TH2REF_TRACE\r\n\tth2ref_text_begin(text_cnt,step_cnt);\r\n#endif\r\n",
     "per-glyph alpha: start of a text object"),
    ("my_inc2/text.cpp",
     "\t\t\t\tif(normal_str) { *normal_str = str[cnt]; normal_str++; }\r\n",
     "\t\t\t\tif(normal_str) { *normal_str = str[cnt]; normal_str++; }\r\n"
     "#ifdef TH2REF_TRACE\r\n\t\t\t\tth2ref_text_glyph(alph2);\r\n#endif\r\n",
     "per-glyph alpha: hankaku"),
    ("my_inc2/text.cpp",
     "\t\t\t\tif(normal_str) { *(WORD*)normal_str = *(WORD*)&str[cnt]; normal_str+=2; }\r\n",
     "\t\t\t\tif(normal_str) { *(WORD*)normal_str = *(WORD*)&str[cnt]; normal_str+=2; }\r\n"
     "#ifdef TH2REF_TRACE\r\n\t\t\t\tth2ref_text_glyph(alph2);\r\n#endif\r\n",
     "per-glyph alpha: zenkaku"),
    # The English release's layout constants.
    #
    # The retail English executable is this same source rebuilt (VS2010,
    # unoptimised, static CRT) with the message window widened - read
    # straight out of its .data, where a resyncing diff against the Japanese
    # retail binary shows these six values as the *only* constant change:
    #
    #     MES_POS_X  48 -> 30     MES_POS_H   10 -> 20
    #     MES_POS_Y  50 -> 25     MES_PICH_H  18 ->  9
    #     MES_POS_W  20 -> 31     SYS_FONT    34 -> 24
    #
    # Without them the reference lays English text out in a box built for
    # Japanese - 38 half-width characters a line against the 57 the
    # translator's own hand-inserted breaks assume - so it wraps mid-word
    # and then silently drops everything past the tenth line.  The harness
    # is aimed at the game being played, so it is built to match it.
    ("ScriptEngine/src/GM_AvgMsg.cpp",
     "int\tMES_POS_X  = (96)/2;\r\n"
     "int\tMES_POS_Y  = 100/2;\r\n"
     "int\tMES_POS_W  = 20;\r\n"
     "int\tMES_POS_H  = 10;\r\n"
     "int\tMES_PICH_W = 0;\t\r\n"
     "int\tMES_PICH_H = 18;\r\n",
     "int\tMES_POS_X  = 30;\r\n"
     "int\tMES_POS_Y  = 25;\r\n"
     "int\tMES_POS_W  = 31;\r\n"
     "int\tMES_POS_H  = 20;\r\n"
     "int\tMES_PICH_W = 0;\t\r\n"
     "int\tMES_PICH_H = 9;\r\n",
     "English release message box"),
    # SYS_FONT has to change for the whole program, not one file.  Doing it
    # with an #undef in GM_AvgMsg.cpp made the message ask for a 24px face
    # while the font loader - in another translation unit, still seeing the
    # header's 34 - had only registered that one, so the text silently
    # stopped drawing at all.
    ("ScriptEngine/src/GM_avg.h",
     "#define SYS_FONT\t34\r\n",
     "#define SYS_FONT\t24\r\n",
     "English release font size"),
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
    # The tree is inconsistent about case and the build runs on a
    # case-sensitive filesystem, so a patched header is written under every
    # spelling its includes use.
    if rel.endswith("GM_avg.h"):
        (GEN/"GM_Avg.h").write_bytes(raw.encode("cp932", "surrogateescape"))
    note = []
    if b or l: note.append(f"{b} asm blocks, {l} asm lines")
    if applied: note.append(f"{applied} patches")
    print(f"  {pathlib.Path(rel).name:18s} {', '.join(note)}")
    total_b += b; total_l += l; total_p += applied
print(f"total: {total_b} asm blocks, {total_l} asm statements, {total_p} patches")
