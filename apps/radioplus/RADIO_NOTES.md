# Radio+ — Phase 0 findings

What the hardware and the two host OSes actually allow, established before any
design work. Everything below is either read out of the RetailOS 1.1.2 image
(`osos.live.bin`, flat load base `0x08000000`), read out of MMIO captured from
the running device, or read out of the SDK sources in this tree. Where something
is inferred rather than observed it says so.

The brief is a full-feature FM app: every tuner register exposed, complete RDS,
recording to WAV with an RDS sidecar, a player for those recordings, background
live buffering that keeps running during playback, and automatic recording of
traffic announcements.

Four things gate that. All four came back positive, but not equally.

---

## 1. Can an app reach the tuner?

**Yes, by two different routes, one per platform.**

The tuner is a Broadcom part driven over HCI, not by memory-mapped registers we
can poke directly. `FM_RDS_Command` is OCF `0x015` in the vendor group, so the
opcode is **`0xFC15`**. Every register in the supplied command list is a
parameter of that one command: an I2C address, a read/write flag, and a payload.
So "support every feature of the tuner chip" reduces to building one command
encoder and 39 register descriptions on top of it — the register table is data,
not code, which is what makes exhaustive coverage tractable at all.

### Linux

`docs-internal/n7g-bt/N31-BLUETOOTH-UART-HCI-FOR-DUMMIES.md` documents the
transport with source anchors: UART port index 1, an MMIO base map, RX waiting on
`base+0x10`, TX waiting on `base+0x18 & 0x200` then writing `base+0x20`. It also
lists vendor commands already proven to round-trip on this hardware — `0xFC18`,
`0xFC01`, `0xFC27` — which matters because it means the transport is not
theoretical and `0xFC15` is the same shape.

### RetailOS

RetailOS has an entire FM subsystem, and it is C++ with RTTI, which means
vtables that can be located and called the same way `sfxPlayer` was for Entrain.
Mangled names in the image:

| Symbol | Meaning |
|---|---|
| `N3ISL8TRFTunerE` | `ISL::TRFTuner` — the tuner base class |
| `N3ISL11IPodRFTunerE` | `ISL::IPodRFTuner` — the concrete tuner |
| `N3ISL12TRFTuner_IAPE` | `ISL::TRFTuner_IAP` — accessory-protocol tuner |
| `N3ISL13TRFTuner_RTXCE` | `ISL::TRFTuner_RTXC` — the RTOS-side tuner |
| `N3ISL21IPodRFTunerPresetListE` | preset list |
| `16CIapLingoRFTuner` | the iAP lingo binding |

Plus `RadioTask` (`0x4bbdf4`) and `MeCCABufferedRDSUpdateTask` (`0x285818`) —
so the OS runs the tuner on its own task, which is the same happy accident that
made Entrain's audio chain work: the right thing already runs in the right
context.

**Not yet done:** locating the `TRFTuner` vtable and mapping its slots. That is
the single piece of RE that decides how much of the RetailOS build is "call the
OS" versus "drive the chip ourselves". It is the same job as `sfxPlayer` and
should be similar effort.

---

## 2. Is the RDS data there, and how complete?

**Complete — more complete than the brief asked for.**

A contiguous descriptor table at `0x473fb8` names every RDS field RetailOS
parses:

```
473fb8 RdsDataAF        alternate frequencies
473fc4 RdsDataDI        decoder identification
473fd0 RdsDataECC       extended country code
473fdc RdsDataPIN       programme item number
473fe8 RdsDataRT        radio text
473ff4 RdsDataGroup     raw group
474004 RdsDataCT        clock time
474010 RdsDataiTunesID  iTunes tagging id
474020 RdsDataTMC       traffic message channel
47402c RdsDataEWS       emergency warning system
474038 RdsDataPTYN      programme type name
474044 RdsDataRTPlus    RadioText+
474054 RdsDataPTY       programme type
474060 RdsDataPSN       programme service name
47406c RdsDataTPAMS     traffic programme / AMS
47407c RdsData:         (prefix used when formatting the above)
```

That covers everything a "full RDS display including all flags" needs, and RT+,
TMC and EWS go beyond it. `TMC` is directly relevant to auto-recording traffic
announcements — though see the honesty note in §5.

There is also `iPod_Control/Device/Radio/Tuner_Metadata.log`,
`Tuner_Readings.log` and `Tuner_Scan.log`, which suggests the OS can be made to
log tuner state to disk — a possible cheap oracle while bringing this up.

---

## 3. Can we capture the audio?

**Yes. The hardware is already doing exactly what the brief describes.**

MMIO captured from the device in three states
(`artifacts/retailos-mmio/fm-playing`, `fm-paused-buffering`,
`fm-buffer-playback`) decodes the PL080 DMA controller as:

| Channel | Path | Peripheral | Flow |
|---|---|---|---|
| ch1 | `0x3d400038` (IIS2 FM RX FIFO) → DRAM | 13 | 2 (peripheral→memory) |
| ch2 | DRAM → `0x3ca00010` (IIS0 TX) | 10 | 1 (memory→peripheral) |

FM audio arrives on a **dedicated I2S controller, IIS2**, separate from the
IIS0 path that drives the speakers. And the decisive observation:

> `EnbldChns` is `0x6` in **all three** states — live, paused-buffering, and
> playing-from-buffer. Live Pause never tears down IIS2 RX. Capture and playback
> run in parallel.

So "a live radio buffer that records in the background even while playing a
recording" is not something we have to invent; it is the configuration the stock
app already runs, and we have its register values. On Linux we can reproduce it
directly. On RetailOS the equivalent already exists as `Radio Live Pause Cache`
(`0x284f7c`), setting `General.RadioLivePause` (`0x7fb7b4`), with
`HandleBufferJumpBackwards` / `Forwards`, `HandleBufferScrubStartBackwards` /
`Forwards` and `HandleBufferScrubStop` (`0x267a40`–`0x267ab8`) as its transport
controls.

**Not yet done:** finding where the OS's capture buffer lives so we can read
samples out of it, versus driving IIS2 ourselves. The paused-buffering capture
shows ch1 writing to `0x0b352380` and ch2 reading from `0x220031d0`, so there are
two distinct DRAM regions to identify.

---

## 4. Can we write the recordings?

**Yes, and with the right API.** `sdk/hb_sdk.h` exposes not just `hb_fs_write`
but a streaming trio:

```c
bool hb_fs_stream_open(const char *path);
bool hb_fs_stream_write(const void *data, uint32_t len);
bool hb_fs_stream_close(void);
```

Streaming is what a recorder needs — a long WAV cannot be buffered whole in
RAM. `hb_fs_mkdir`, `hb_fs_dir_open`/`next`/`close`, `hb_fs_size`, `hb_fs_remove`
are all present too, so the recordings browser and the sidecar files are
straightforward. `sdk/hb_record.c` already writes multi-file output to
`/recNNNN/`, so there is a working precedent in-tree for the pattern.

The WAV writer itself is already solved: Entrain's `core/wavout.c` is a pure
header builder with no I/O, and is directly reusable.

---

## 5. What this does *not* establish

Being straight about the gaps, because the feature list leans on them:

- **The `TRFTuner` vtable is unmapped.** Everything on the RetailOS side that is
  "call the OS" rather than "drive the chip" is blocked on that one piece of RE.
- **Whether an app can send raw HCI under RetailOS is unproven.** The OS clearly
  can; whether it exposes a path we can borrow, or whether we go through
  `TRFTuner` instead, is unknown. The Linux path has no such doubt.
- **The capture buffer address is unknown**, so recording on RetailOS is not yet
  demonstrated end to end. Linux is the safer first target for the recorder.
- **Traffic announcement auto-record depends on broadcast content.** The TP/TA
  flags and `RdsDataTMC` are decodable, and `I2C_RDS_BLKB_MATCH`/`MASK` can be
  programmed to interrupt on the right group — but whether local stations
  actually transmit TA is not something the firmware can tell us. The feature
  should be built so it is obviously idle rather than obviously broken when no
  station signals TA.
- **Antenna.** `I2C_FM_ANTENNA_SELECTION` distinguishes internal from external;
  on this hardware the headphone cable is the aerial, so recording while nothing
  is plugged in may be useless in practice. Worth surfacing in the UI rather
  than letting a user record silence.

---

## 6. Consequences for the design

1. **The register table is data.** One `0xFC15` encoder plus a description of all
   39 registers — type, bit meanings, read length, range — gets exhaustive
   coverage without 39 hand-written screens. The read-length rules in the command
   spec are themselves a table and should be transcribed verbatim rather than
   summarised, because several are marked TBD upstream and guessing them wrongly
   is a bus transaction that fails.
2. **Split the same way Entrain did.** `core/` pure C99 with no SDK and no LVGL —
   the command encoder, the RDS decoder, the WAV writer, the recording index —
   so all of it is testable on the desktop where the tuner is not. `platform/`
   for the two transports. That worked, and RDS decoding especially wants real
   tests against captured group data.
3. **Linux first for recording, RetailOS first for tuning.** The Linux side has a
   documented transport and we can drive the DMA ourselves; the RetailOS side has
   a whole subsystem to borrow but needs the vtable found first. They converge.
4. **Capture RDS to the sidecar as raw groups plus decoded fields.** Raw groups
   mean a recording can be re-decoded later when the decoder improves, which it
   will.

---

## 7. Status

Scaffolded (`./start new radioplus`, LVGL, title "Radio+"). Nothing implemented
yet — this document is the Phase 0 deliverable and the next step is the
`TRFTuner` vtable hunt.

---

# Phase 0b — the TRFTuner vtable hunt

The gap flagged in §5 is now closed enough to design against. The tuner vtables
are located, their slots are classified by shape, and part of the object layout
falls out for free.

## The technique

The tuner classes carry RTTI, and the Itanium C++ ABI lays RTTI out in a fixed
shape, so a vtable can be reached from nothing but its class name:

```
type_info   [ vptr ][ __name -> "N3ISL8TRFTunerE" ][ ...base info... ]
vtable      [ offset_to_top ][ type_info* ][ vfn0 ][ vfn1 ] ...
```

Find the name string; find the word pointing at it (that is `type_info+4`); find
the word pointing at the type_info (that is `vtable+4`); the virtual functions
start immediately after. Every step is checkable — a real vtable is a run of
plausible code pointers, and anything else means the chain went somewhere else.
No guessing at any point, which matters, because guessing at RetailOS internals
has already cost this project two bad builds on the Entrain side.

`re/vtable.py` does the walk, `re/vtdiff.py` the comparison, `re/vtmap.sh` the
per-slot classification, `re/findref.py` finds accessors of a struct offset, and
`re/dis.sh` disassembles at a virtual address. All re-runnable.

## What was found

| Class | type_info | vtable |
|---|---|---|
| `ISL::TRFTuner` | `0x0878bbc0` | **`0x087f0050`** |
| `ISL::TRFTuner_IAP` | `0x0878b590` | **`0x087ecfd8`** |
| `ISL::TRFTuner_RTXC` | `0x0878b62c` | **`0x087ed41c`** |
| `TRFTunerPresetList` | `0x08788600` | **`0x087a8a30`** |
| `CIapLingoRFTuner` | `0x087880c8` | **`0x087a2d80`** |
| `ISL::IPodRFTuner` | `0x0878b4f8` | *none found* |

`TRFTuner` is **abstract**: slots 4, 5, 11 and 12 are null. So it declares an
interface and implements most of it, and the concrete tuners fill in the rest.

`TRFTuner_RTXC` is the one that matters — RTXC is the RTOS-side path to the
built-in chip, where `_IAP` is the accessory-protocol tuner for external
hardware. Both are worth having: the shared slots are identical in all three, so
anything reached through `TRFTuner`'s own implementation works either way.

`IPodRFTuner` has a type_info but nothing points at it from a vtable. Either it
is abstract with no emitted table, or it is only ever named, not instantiated.
Unresolved, and not obviously in the way.

## Slot map, 38 slots

Diffing the three tables separates inherited plumbing from per-transport work:

- **Identical in all three** (TRFTuner implements it once):
  2, 3, 6, 7, 8, 9, 10, 13, 14, 15, 16, 17, 19, 20, 21, 22, 23, 24, 25, 26, 28,
  33, 34, 35, 37
- **Overridden per implementation** (the transport primitives):
  0, 1, 18, 27, 29, 30, 31, 32, 36
- **Pure virtual**: 4, 5, 11, 12

Slots 0 and 1 are the destructor pair — `TRFTuner_RTXC` slot 0 writes a vptr and
a second pointer at `+0x44`, and slot 1 calls slot 0 then jumps to the
deallocator, which is exactly the complete/deleting destructor shape.

Several RTXC overrides are constant predicates, which is what a capability query
compiles to:

| Slot | `TRFTuner_RTXC` |
|---|---|
| 18 | `movs r0,#1; bx lr` — returns true |
| 29 | `movs r0,#0; bx lr` — returns false |
| 30 | `movs r0,#1; bx lr` — returns true |
| 31 | `movs r0,#0; bx lr` — returns false |
| 32 | `b.w 0x8472ea4` — tail-call into shared code |
| 27 | real work: 40 bytes of stack, calls `0x8416c5c` |

## Object layout, so far

Read straight off the accessors rather than inferred:

| Offset | Width | Evidence |
|---|---|---|
| `+0x4C` | byte | read in slot 20's neighbourhood |
| `+0x4E` | byte | slot 33 is `ldrb.w r0,[r0,#0x4E]; bx lr` |
| `+0x50`, `+0x54` | words | slot 37 copies both into a caller-supplied struct |
| `+0x390` | byte | written by a setter guarded on `r1 == 1` |
| `+0x394` | pointer | slot 26 loads it and branches on null |
| `+0x398` | pointer | slots 28, 34, 35 load it and fall back to a literal when null — a delegate |
| `+0x39C` | byte | **the state**: eight call sites compare it against 0, 1 and 2 |
| `+0x39E` | byte | slot 20 is a plain getter |
| `+0x3A0` | word | slot 21 is `ldr.w r0,[r0,#0x3A0]; bx lr` |

`+0x39C == 2` is the guard slots 22, 23, 24 and 25 all check before doing
anything, so 2 is the powered/ready state.

## Confidence, honestly

**Solid:** the vtable addresses, which slots are shared versus overridden, the
pure virtuals, the destructor pair, and every offset in the layout table — those
are read directly out of instructions.

**Not established:** what any slot is *called*. Shape is not meaning: slot 21
being a word getter at `+0x3A0` makes it a plausible frequency, but nothing yet
proves it, and this project has already been bitten twice by a plausible reading
of RetailOS that turned out wrong. No slot gets a name in code until a caller or
a device observation confirms it.

Worth recording as a trap: scanning for a struct offset finds *every* class that
uses it. The byte accessors of `+0x3A0` around `0x0841xxxx` are a different
cluster from the `TRFTuner` methods at `0x0847xxxx` and are very likely a
different class entirely. Offsets alone do not identify an object.

## Next

1. Name the slots from their callers — `TSilverRadioTunerBarView` and
   `TRadioTagListCntlr` drive this object and their call sites will say which
   slot means what.
2. Find how `TRFTuner_RTXC` reaches the chip: slot 27 and `0x8416c5c` are the
   thread to pull, and that is where `0xFC15` should surface.
3. Locate the Live Pause cache buffer, using `Radio Live Pause Cache`
   (`0x284f7c`) and the `HandleBuffer*` handlers (`0x267a40`–`0x267ab8`).
