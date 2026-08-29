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

---

# Phase 0c — naming the slots: what worked, what did not

## Naming from log strings is impossible here, and that is a finding

The obvious way to name a vtable slot is to find a caller that logs what it is
doing, and RetailOS looks like it should oblige. The strings are there and they
name the operations outright:

```
2c5db4  We have a frequency event
2c5dd0  [Tune] [%s] New frequency is %d kHz (old was %d)
2c5e1c  [Seek] [%s] New frequency is %d kHz (old was %d)
2c5e68  Our RSSI is %d (original = 0x%x)
2c5e8c  Our SNR is %d (original = 0x%x)
2c5700  fnRssiThreshold: %d. fnNoiseThreshold: %d
1b4de4  "frequency " / " : rssi " / " : snr "     (the Tuner_Readings.log format)
1b3428  UNKNOWN / ACCESSORY / INTERNAL            (the "Tuner Used :" enum)
```

**None of them is referenced by address.** Not one. And that is not a broken
search: the same tool finds the type_info name reference at `0x0878bbc4` and the
`TRFTuner` vptr reference at `0x08027a64` immediately.

The reason is that RetailOS logs by index, not by pointer. `0x0841acf4` is the
dispatcher: it takes an event id, bails with `103` if a global is clear, and
otherwise compares the id against a whitelist — `0x83`, `0x36`, `0x84`, `0x85`,
`0x86`, `0x87`, `0x89`, `0x35`, `0x75`, `0x82`, `0x97`, `0xb0`, `0xa3`, `0x61`,
… — all branching to one place. Call sites pass a small constant id and the
arguments; the text is looked up elsewhere. So the strings are data in a table
and the code never mentions them.

That also explains the `RdsData*` block: a 12-byte stride with no individual
references is a table indexed by field number, not a set of pointers.

**Consequence for this project:** slots cannot be named from strings. They get
named from call sites or from device observation, and until then they stay
unnamed. `INTERNAL`/`ACCESSORY` does independently confirm the two-tuner split
(RTXC and IAP respectively).

## The FM opcode appears exactly once

`0xFC15` is materialised in the whole image exactly once, at **`0x0854ce72`**,
as `movw r0, #0xFC15`. `re/findimm.py` finds it by searching for the MOVW
*encoding*, which is necessary because a 16-bit immediate lives in the
instruction rather than in a literal pool — searching for the bytes finds
nothing.

The function around it:

```
854ce54: ldr  r0,[pc]        -> global 0x0896dd00 ; deref, bail if null
854ce5c: ldr  r0,[pc]        -> buffer 0x08d09f74 (RAM, not firmware data)
854ce5e: ldrd r1,r2,[r0]     \
854ce62: ldr  r0,[r0,#8]      >  12 bytes of live payload onto the stack
854ce64: strd r1,r2,[sp]     /
854ce68: add.w r2, sp, #1     ; payload pointer, one byte in
854ce6c: ldrb.w r1,[sp]       ; that first byte is the payload LENGTH
854ce72: movw r0, #0xFC15     ; opcode
854ce76: bl   0x80f731e
854ce7a: cbz  r0, <return 0>  ; zero is success
854ce86: bl   0x841bacc       ; else trace(0x41, line 503) and return 114
```

**Correction to a reading I made an hour ago:** I took `0x080f731e` for the HCI
send. It is not. It is three instructions of argument shuffling in front of
`0x0841acf4` with event id `0xB4` — a *trace* wrapper. So `0x0854ce54` logs an
FM command; it does not transmit one. Recording the mistake because the same
over-reading of RetailOS has now cost this project twice on the Entrain side,
and the pattern is always the same: a function whose shape fits the hypothesis,
adopted before following it one level down.

What the site is still worth:

- It **localises the FM HCI code** to `0x0854xxxx`, which is a small
  neighbourhood to search for the real send.
- It gives the **command shape**: `(opcode, length, payload)` with the length as
  a leading byte of the buffer.
- It names two anchors, and bounds-checking them corrected a second reading.
  Both are runtime state rather than firmware constants:
  - **`0x0896dd00`** — in the image, but all zeros, so BSS. This is the global
    checked non-null before anything is attempted: a handle populated when the
    FM stack comes up.
  - **`0x08d09f74`** — **outside the image entirely**, so a RAM address. Not a
    canned command template as first written up, but a live buffer built at run
    time. Which means the payload cannot be recovered statically at all, and the
    command shape has to come from the register table or from the device.

  Cheap habit worth keeping: an address that looks like firmware data is worth
  checking against the image bounds before any claim is built on it.

## Where this leaves the two builds

Nothing found so far blocks the Linux target: that path goes through the
documented UART HCI transport and does not need any of this.

For RetailOS there are now two candidate routes and neither is proven:

1. **Call `TRFTuner` through its vtable.** Located and mapped, but unnamed, and
   the class is abstract with concrete leaves that are themselves abstract at
   slots 4, 5, 11 and 12 — so an instance has to be obtained rather than
   constructed. The vptr reference at `0x08027a64` is the thread.
2. **Find the real HCI send and issue `0xFC15` directly.** Strictly better if it
   exists, because it exposes every register in the command list rather than
   whatever `TRFTuner` chose to abstract. The search area is now small.

Route 2 is worth one more pass before committing to route 1.

## Next

1. Disassemble outward from `0x0854ce54` for the function that actually
   transmits, and identify the readiness global at `0x0896dd00`.
2. Decode the 12-byte template at `0x08d09f74` — it is a real `FM_RDS_Command`
   payload and should parse against the register table, which would confirm the
   whole shape.
3. Only then decide between calling `TRFTuner` and driving `0xFC15` directly.

---

# Phase 0d — route 2: the real HCI send, verified on 1.1.2

Route 2 is answered. Radio+ can issue `0xFC15` itself on RetailOS, which means
every register in the command list is reachable rather than whatever `TRFTuner`
chose to abstract.

## The API

All three verified in `osos.live.bin` (1.1.2), which is what the device runs:

| Address | Signature | Notes |
|---|---|---|
| **`0x0854F878`** | `uart_open(port=1, baud=115200)` | three instructions, tail-calls `0x08172BCC`; returns the handle in r0, null on failure |
| **`0x08410820`** | `hci_send(handle, buf, len)` | returns 0 on success, 7 if the handle is null; 3000 ms timeout, length passed by reference |
| **`0x0840DA00`** | `hci_wait_event(14, handle, buf, buflen)` | checks `buf[0] == 4`, the H4 event packet type |

The call pattern, straight from the bring-up sequence at `0x0854f584`:

```
854f59a: bl   0x854f878     ; handle = uart_open(1, 115200)
854f59e: movs.w r8, r0      ; null -> bail out
854f5ac: ldr  r1,[pc]       ; -> command table 0x08754d98
854f5ae: movs r2, #4        ; length
854f5b0: mov  r0, r8
854f5b2: bl   0x8410820     ; hci_send(handle, cmd, 4)
854f5b6: mov.w sl, #2048    ; event buffer size
854f5ba: movs r0, #14
854f5be: mov  r2, sp        ; event buffer
854f5c0: mov  r1, r8
854f5c2: bl   0x840da00     ; hci_wait_event(14, handle, buf, 2048)
854f5cc: adds r1, r1, #4    ; next command in the table, send again
```

## Why this is trustworthy

The command table at `0x08754d98` decodes to opcodes that are recognisable
without any interpretation on our part:

```
01 03 0c 00   ->  0x0C03  HCI_Reset
01 01 10 00   ->  0x1001  HCI_Read_Local_Version_Information
01 09 10 00   ->  0x1009  HCI_Read_BD_ADDR
```

Three standard HCI opcodes in the canonical bring-up order. That confirms the H4
framing from the firmware side — `01 <ocf_lo> <ogf_hi> <len> <payload...>` —
rather than us asserting it. The port index and baud rate also agree with the
`docs-internal` Bluetooth notes, which were reverse-engineered separately.

So an FM register write becomes:

```
01 15 FC <len> <I2C_address> <Read_Write_Mode> <payload...>
```

and a read is the same with `Read_Write_Mode = 1`, answered through
`hci_wait_event`. Every register in the command list is reachable this way.

## The version trap, which cost a wrong turn

The Bluetooth notes in `docs-internal` were written against `osos.dec.bin`,
which is **1.0.2**. The device runs **1.1.2**. Addresses do not carry across:
`sub_422794` from those notes disassembles to arithmetic noise in 1.1.2, which
is exactly what happened on the first attempt and briefly looked like the notes
being wrong rather than the image being the wrong one.

`re/port.py` carries an address between builds by code signature. It takes the
bytes at the 1.0.2 address, masks the operands that are allowed to move — BL,
B.W and BLX pairs, and PC-relative loads, all of which encode distances rather
than destinations — and looks for the same code in 1.1.2:

| Function | 1.0.2 | 1.1.2 |
|---|---|---|
| `hci_send` | `0x08422794` | **`0x08410820`** (unique on 16 bytes) |
| `hci_wait_event` | `0x0841F4C2` | **`0x0840DA00`** (unique on 40 bytes) |
| handle acquisition | `0x08570360` | no match; the function changed, found through callers instead |

Any address taken from those notes has to go through this first. Reading a 1.0.2
address in the 1.1.2 image produces plausible-looking instructions, not an
error, which is the whole danger.

## A tool bug worth recording

`re/callers.py` decodes every BL and BLX in the image to find callers, because
Thumb-2 encodes a call as a distance and the callee address never appears
anywhere to be searched for. Its first version reported **zero** callers for the
send — and zero for the trace dispatcher, which plainly has hundreds. That
second number is what made it obviously a bug rather than a finding.

Bits 15 down to 12 of the second halfword are `1, 1, J1, 1`, so the nibble is
`0xD` or `0xF` for BL depending on `J1`, and `0xC` or `0xE` for BLX. Matching
the nibble against `0xD` and `0xC` alone silently drops about half of all
branches. Fixed by masking `J1` out and testing bits 15, 14 and 12.

The habit that caught it: run a new search tool against something whose answer
is already known before believing a negative result from it.

## Caution before this is called on hardware

`uart_open` opens the transport. If the OS Bluetooth stack is already up it owns
that UART, and opening it a second time is a conflict rather than a shortcut.
The FM path should reuse the existing handle when the stack is running and only
open one when it is not. The readiness global noted earlier — `0x0896dd00`, BSS,
null-checked before anything is attempted — is very likely exactly that
distinction, and should be resolved before any of this runs on a live device.

## Consequence for the design

Route 2 wins. The RetailOS backend targets `0xFC15` directly, which means both
platform backends speak the same language — a raw `FM_RDS_Command` — so the
whole register table becomes shared, testable, pure C99 in `core/`, exactly the
way Entrain's DSP was.

The `TRFTuner` work is not wasted. It stays the fallback if issuing HCI
alongside a running OS stack turns out to be unsafe, and `TRFTunerPresetList` is
still likely the cheapest way to read the presets the stock app has saved.

---

# Phase 0e — UART ownership: do not open it, borrow it

Resolved, and the answer is better than expected: there is a send entry point
that fetches the live handle itself, so Radio+ never touches the transport.

## What the readiness global actually is

`0x0896dd00` is not a handle. It is a struct, and a teardown at `0x080da068`
checks and clears three fields:

```
80da06c: ldr r1,[r0,#12] ; cbz -> fail
80da070: ldr r1,[r0,#16] ; cbz -> fail
80da074: ldr r1,[r0,#20] ; cbz -> fail
80da078: store 0 to +12, +16, +20 ; return 0
```

and a registration at `0x08416b84` appends 16-byte records to a table, bumps a
count, and tail-calls the FM function when the count first reaches 1. So it is a
client/callback registry for FM, not the transport.

## The send to use

```
8133ed4: mov r2, r1         ; len = arg1
8133ed6: mov r1, r0         ; buf = arg0
8133ed8: ldr r0,[pc]        ; -> 0x0896d8e0
8133ee0: ldr r0,[r0,#20]    ; the live handle
8133ee2: bl  0x8410820      ; hci_send(handle, buf, len)
8133ee6: cbz r0 -> success
8133ee8: cmp r0, #31 ; beq  ; 31 handled separately
8133eec: movs r4, #114      ; otherwise error
```

| Address | Meaning |
|---|---|
| **`0x08133ED4`** | `hci_send_current(buf, len)` — two arguments, finds the handle itself |
| **`0x0896D8E0`** | the transport struct (BSS) |
| **`0x0896D8F4`** | the live handle, struct + 20 — non-null means the transport is up |

`0x08133ED4` has **zero BL callers**, so it is reached through a pointer: it is
the transport driver's `send` method, which matches the `docs-internal` note
that the HCI driver is registered with a vtable-like interface. That is exactly
what makes it the right thing to call — it is the OS's own send, used the way
the OS uses it.

So the plan is: **never call `uart_open`.** Read `0x0896D8F4`; if it is null the
stack is not up and there is nothing to talk to, and if it is non-null call
`0x08133ED4(buf, len)`. `uart_open` at `0x0854F878` has exactly one caller — the
bring-up sequence — and opening a second time would be a conflict rather than a
shortcut.

## The event format, from the firmware

The bring-up checks its own command-complete reply byte by byte at `0x0854f82a`:

```
sp[3] == 0x01     ; num_hci_command_packets
sp[4] == 0x18     \ opcode, little endian
sp[5] == 0xFC     /
sp[6] == 0x00     ; status
```

So an event is:

```
[0] 04            H4 event packet type   (hci_wait_event checks this itself)
[1] 0E            Command Complete
[2] plen
[3] 01            num_hci_command_packets
[4] ocf_lo
[5] ogf_hi
[6] status
[7..] return parameters
```

**A register read comes back at offset 7.** That is the whole read path, and it
came from the firmware checking its own reply rather than from us guessing.

## The hazard this exposes, which is not solved

Writes look safe. Reads do not, and it would be dishonest to call this finished.

`hci_wait_event` pulls from the transport directly. While the OS Bluetooth stack
is running it has its own RX path consuming events, so a command-complete for
our `0xFC15` may be dispatched to the OS before we see it — or worse, we may
consume an event the OS was waiting for. That is a race we do not control, and
it gets more likely the busier the stack is.

Options, none yet tested:

1. **Write-only through HCI, read through `TRFTuner`.** The vtable is already
   mapped, and reads are the smaller half of the feature set.
2. **Hook the event dispatcher** and filter command-completes for opcode
   `0xFC15` out of the stream. Cleanest if it works, and `hb_silver_patch_function`
   in the SDK exists precisely to patch a non-virtual function.
3. **Only send when the stack is idle**, i.e. FM is up but no Bluetooth link is
   active. Narrow, and not something the app can guarantee.

This has to be settled on the device rather than in the disassembler, and the
first build should therefore treat reads as unproven: send writes, and display
what comes back only when it can be correlated with a request. The register
table and encoder below are unaffected either way — they are pure data and pure
functions, and they are correct regardless of which transport answers.
