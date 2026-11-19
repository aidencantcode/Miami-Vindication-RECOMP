# Recompiler survey (Phase 4.1)

Library: `libGangster2.so`; `.text` 3,185,996 bytes at `0x274c98`; `.plt` 3,500 bytes (290 entries).

## Code map (mapping symbols in `.text`)

- `$a`: 12,544 bytes
- `$d`: 164,928 bytes
- `$t`: 3,008,524 bytes

## Functions

- 24,199 in `.text`: 23,415 Thumb, 784 ARM
- names with aliases: 1 addresses
- Thumb instructions outside any function symbol: 5,870

## Thumb decode

- 1,403,141 instructions, **5 undefined**
  - `0x2ae98c` raw `ff1c` in _ZThn8_N10HudElement12processTouchEl
  - `0x2b2710` raw `ff1c` in _ZThn4_NK11RetroEffect15getDrawingLayerEv
  - `0x2b2cdc` raw `ff1c` in _ZThn8_NK9HudScreen6isUsedEv
  - `0x2b4484` raw `ff1c` in _ZThn8_N11SwipeButton19processTouchReleaseEv
  - `0x3da6d8` raw `ff1c` in _ZThn4_N6glitch5scene19CBillboardSceneNode19onRegisterSceneNodeEv

### Histogram

| kind | count |
|---|---|
| `hmov` | 135,464 |
| `ldr_i` | 133,302 |
| `add_i3` | 129,606 |
| `mov_i` | 106,673 |
| `bcond` | 83,427 |
| `str_i` | 72,737 |
| `bl` | 60,611 |
| `cmp_i` | 53,257 |
| `lsl_i` | 49,296 |
| `ldr_sp` | 46,833 |
| `ldr_lit` | 45,857 |
| `blx` | 40,510 |
| `b` | 39,899 |
| `add_i` | 39,792 |
| `ldr_r` | 38,994 |
| `str_sp` | 38,708 |
| `add_r` | 30,841 |
| `pop` | 23,953 |
| `push` | 23,934 |
| `alu.cmp` | 19,302 |
| `hadd` | 18,227 |
| `str_r` | 15,050 |
| `ldrb_i` | 15,021 |
| `blx_r` | 13,083 |
| `strb_i` | 12,972 |
| `add_sp` | 12,558 |
| `sub_i` | 10,324 |
| `sub_r` | 7,565 |
| `strb_r` | 6,223 |
| `sp_add` | 5,983 |
| `sp_sub` | 5,965 |
| `asr_i` | 5,618 |
| `bx` | 5,070 |
| `lsr_i` | 5,041 |
| `ldrb_r` | 4,275 |
| `hcmp` | 4,267 |
| `alu.and` | 4,221 |
| `alu.neg` | 4,110 |
| `ldrh_i` | 3,974 |
| `alu.orr` | 3,949 |
| `alu.mul` | 3,703 |
| `sub_i3` | 2,942 |
| `alu.eor` | 2,543 |
| `strh_i` | 2,486 |
| `ldrsb_r` | 1,976 |
| `stmia` | 1,963 |
| `ldmia` | 1,761 |
| `ldrh_r` | 1,198 |
| `alu.lsl` | 1,180 |
| `strh_r` | 1,025 |
| `alu.bic` | 1,011 |
| `ldrsh_r` | 971 |
| `alu.sbc` | 904 |
| `alu.tst` | 887 |
| `alu.adc` | 800 |
| `alu.lsr` | 724 |
| `alu.asr` | 342 |
| `alu.mvn` | 215 |
| `alu.ror` | 13 |
| `undef` | 5 |

## Control flow

- branch.intra: 123,306
- pop {..pc} (return): 18,331
- blx reg (indirect call): 13,083
- hadd with pc operand: 8,979
- bx lr (return): 5,013
- mov pc, reg: 227
- bx reg (indirect jump): 57
- bl->func_start: 58,401
- blx->plt: 40,129
- bl->intra: 2,210
- blx->arm_func: 381
- b->mid_function: 18
- b->func_start: 2
- distinct mid-function entry points (targets of calls/branches from elsewhere): 18

## ARM code (capstone mnemonics)

- `bx`: 782
- `sub`: 781
- `ldr`: 778
- `add`: 778
- `mov`: 3
- `lsls`: 2
- `bne`: 2
- `bhs`: 1
- `cmp`: 1
- `blo`: 1
- `subs`: 1
- `bmi`: 1
- `lsl`: 1
- `orr`: 1
- `lsr`: 1
- `cmn`: 1
- `mvn`: 1

## Analysis (tools/recomp/analyze.py)

- jump tables recovered: **227 of 227** (4,845 targets); unresolved: 0
- ARM thunks recognised: **783 of 784**
- functions with extra entry points: 0 (0 entries)
- per-function problems: 4
  - pfr_glyph_load_rec: jump table at 0x5140a8: unbounded guess, 14 entries
  - parse_dict: jump table at 0x52acea: unbounded guess, 6 entries
  - ft_gzip_file_fill_output: jump table at 0x53bdb4: unbounded guess, 15 entries
  - __aeabi_f2uiz: ARM function is not a thunk
- functions whose last instruction is not a return/jump: 727 (blx 650, bl 77)
  - _ZL12init_mparamsv: ends with blx
  - _ZNSt3mapISsjSt4lessISsESaISt4pairIKSsjEEEixISsEERjRKT_.clone.13: ends with blx
  - _ZNSt3mapISbIcSt11char_traitsIcEN6glitch4core10SAllocatorIcLNS2_6memory13E_MEMOR: ends with blx
  - _ZN7gameswfL26get_color_transform_memberERKNS_10tu_stringiE: ends with blx
  - _ZL11AddSentinelP13GLUtesselatord: ends with blx
  - _ZL14AddRegionBelowP13GLUtesselatorP12ActiveRegionP11GLUhalfEdge: ends with blx
  - _ZL19SpliceMergeVerticesP13GLUtesselatorP11GLUhalfEdgeS2_: ends with blx
  - _ZL19CheckForRightSpliceP13GLUtesselatorP12ActiveRegion: ends with blx
  - _ZL16WalkDirtyRegionsP13GLUtesselatorP12ActiveRegion: ends with blx
  - _ZL17CheckForIntersectP13GLUtesselatorP12ActiveRegion: ends with blx
  - _ZL13AddRightEdgesP13GLUtesselatorP12ActiveRegionP11GLUhalfEdgeS4_S4_c: ends with blx
  - _ZL10SweepEventP13GLUtesselatorP9GLUvertex: ends with blx

### `bx reg` (not lr) sites

- bx r3 after sp_add: 57
