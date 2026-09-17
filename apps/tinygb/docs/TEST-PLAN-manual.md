# Manual Test Plan

## Test environment

- One iPod nano 7G. For RetailOS tests: ipod_sun_untethered restored, NanoApps resident installed from the fork, iPod attached to a Linux machine or WSL host that `./start` can drive. For N31 tests: the N31 image booted, ssh at `root@192.168.7.2` with `artifacts/linux-n31/n31_id`.
- Cartridges (the tester's own): Tetris (World) (Rev 1) (no SRAM), Super Mario Land 2 (SRAM, MBC1), Pokemon Red (1 MiB, MBC3, SRAM), Link's Awakening (SRAM). Plus the fetched `dmg-acid2.gb`.
- Headphones, for the audio tests.

## Setup and reset

- RetailOS: `./start install tinygb` from the NanoApps fork; copy cartridges to `/Apps/Data/TinyGB/roms/` in disk mode or with `./start` data copy. Reset: delete `/Apps/Data/TinyGB/` in disk mode.
- N31: `bash tools/linux-n31/build-n31-apps.sh tinygb` then `install-n31os-disk.ps1`. Reset: delete `.sav`, `.st0`, and `~/.config/tinygb` on the volume.
- Record the build stamp from the About row before every session.

## Smoke

| ID | Area | Steps | Expected | Status |
|---|---|---|---|---|
| MAN-SMOKE-001 | ROS launch | Tap TinyGB on the Home Screen | Library list within 1 s, cartridges sorted, no leftover pixels from the previous app | Not Run |
| MAN-SMOKE-002 | ROS play | Tap Tetris | Title screen within 2 s, music playing, pad visible below | Not Run |
| MAN-SMOKE-003 | N31 launch | Open TinyGB from the launcher | LVGL picker; Vol Up/Down move, Play starts | Not Run |
| MAN-SMOKE-004 | Home | Press Home during play on each target | ROS: back to Home Screen, no sound continues. N31: pause menu | Not Run |

## Picture

| ID | Area | Steps | Expected | Status |
|---|---|---|---|---|
| MAN-ROS-001 | acid2 | Start dmg-acid2, wait 2 s, triple-click Home for a screenshot, `./start pull media`, run `tools/acid2check.py` on the top 240 x 216 after downscaling per `tg_headless -S` comparison script | Byte-identical to host scaled output | Not Run |
| MAN-N31-001 | acid2 | Same via `tools/grab-screen.sh` | Byte-identical | Not Run |
| MAN-ROS-002 | Scaling | Settings -> Scaling -> Sharp, Resume | Hard 2:3 edges; no reset of the game | Not Run |
| MAN-ROS-003 | Palette | Cycle all five palettes during Tetris | Each repaints fully within one tick; no stale rows | Not Run |
| MAN-ROS-004 | Row skip | Leave Tetris on the title screen 60 s | No flicker, no tearing lines; fps overlay steady | Not Run |

## Input

| ID | Area | Steps | Expected | Status |
|---|---|---|---|---|
| MAN-ROS-010 | Chord | Super Mario Land: hold Right on the pad and tap A with another finger | Runs and jumps | Not Run |
| MAN-ROS-011 | Diagonal | Zelda: press a d-pad corner | Link walks diagonally | Not Run |
| MAN-ROS-012 | Buttons | Vol Up as A, Vol Down as B, both together | Both register; OS volume HUD may appear | Not Run |
| MAN-ROS-013 | Play/Pause | Press Play/Pause during play | Nothing in-game; if Music starts underneath, it is paused within 250 ms | Not Run |
| MAN-ROS-014 | Tilt | Settings -> Tilt on; hold upright; lean left/right/forward/back | Directions with hysteresis, no chatter; resting holds nothing | Not Run |
| MAN-ROS-015 | Menu pill | Tap the pill under the pad | Pause menu; audio silent within 300 ms, no click | Not Run |
| MAN-N31-010 | Keys | Vol Up / Down / Play | A / B / Start | Not Run |
| MAN-N31-011 | Tilt | As MAN-ROS-014 | Same | Not Run |

## Audio

| ID | Area | Steps | Expected | Status |
|---|---|---|---|---|
| MAN-AUD-001 | Tone | Build with the tone switch; play 60 s; record through headphones into a PC | Continuous 440 Hz, no gaps or clicks in the waveform | Not Run |
| MAN-AUD-002 | Music | Tetris title music 5 min | No gap or click; overlay underruns 0, restarts 0 | Not Run |
| MAN-AUD-003 | Pause | Open and close the menu 10 times during music | No click; pitch unchanged after | Not Run |
| MAN-AUD-004 | Latency | Tetris: rotate a piece; compare the sound delay by ear to the N31 build | Under 250 ms; not obviously worse than N31 | Not Run |
| MAN-AUD-005 | Music app | Press Play/Pause so RetailOS starts Music; wait | Game audio resumes after the starvation restart; music paused | Not Run |
| MAN-N31-020 | Rate | Play 5 min; read the exit report | Measured Hz within 0.1 % of 48000; restarts 0 | Not Run |

## Persistence

| ID | Area | Steps | Expected | Status |
|---|---|---|---|---|
| MAN-DATA-001 | Battery | Pokemon Red: save in-game; hold Power 3 s after (ROS) or 6 s (N31); reboot; start | Save present | Not Run |
| MAN-DATA-002 | Foreign save | Copy a SameBoy `.sav` for the same cartridge; start | Same save slots | Not Run |
| MAN-DATA-003 | Wrong size | Truncate a `.sav`; start | Fresh game; file unchanged; About page names the refusal | Not Run |
| MAN-DATA-004 | State | Save state mid-level; play on; Load state | Back at the saved moment; SRAM consistent | Not Run |
| MAN-DATA-005 | Resume | ROS: open the menu, press Home; relaunch; Library shows Resume for that cartridge; choose it | Continues from the pause | Not Run |
| MAN-DATA-006 | Settings | Change palette and scaling; Home; relaunch | Settings kept | Not Run |
| MAN-DATA-007 | Empty | Remove all cartridges; launch | Help text naming `/Apps/Data/TinyGB/roms` | Not Run |
| MAN-DATA-008 | Too big | Put a 2 MiB `.gbc` in the folder | Listed greyed with size; tap does nothing | Not Run |

## Lifecycle and memory

| ID | Area | Steps | Expected | Status |
|---|---|---|---|---|
| MAN-ROS-020 | Wake | Play with tilt only, no touch, 60 s | Panel does not dim | Not Run |
| MAN-ROS-021 | Idle | Sit in the Library 60 s | Panel dims per the OS timer | Not Run |
| MAN-ROS-022 | Leak | Launch and Home 10 times; `./start trace` | Heap free at init drops by no more than the audio slot total per launch, and recovers after reboot | Not Run |
| MAN-ROS-023 | Soak | One hour of Tetris demo mode | Still running; no reboot; overlay counters stable | Not Run |
| MAN-N31-030 | Soak | One hour | Same; exit report sane | Not Run |
| MAN-ROS-024 | Arena | First launch after install; `./start trace` | Init logged `heap_largest` and `heap_free`; both above the ROM window + audio + 256 KiB | Not Run |

## Destructive

| ID | Area | Steps | Expected | Status |
|---|---|---|---|---|
| MAN-DESTR-001 | Full volume | Fill the volume; save in-game | Write fails, reported on About; game continues; retries after space is freed | Not Run |
| MAN-DESTR-002 | Bad ROM | Put a text file named `x.gb` in the folder | Listed; starting it returns to the library with "not a cartridge" | Not Run |
| MAN-DESTR-003 | Corrupt state | Overwrite `.st0` with garbage; Load state | Refused; game keeps running | Not Run |

## Regression checklist (run before every tag)

MAN-SMOKE-001..004, MAN-ROS-001, MAN-N31-001, MAN-ROS-010, MAN-AUD-002, MAN-AUD-003, MAN-DATA-001, MAN-DATA-004, MAN-ROS-022.
