# Measuring the practice recordings — where this got to

Paused mid-way. This is the state, so it can be picked up without re-deriving it.

## Source

`T:\downloads\complete\Misc\Hemi-Sync - The Gateway Experience [FLAC] (corrected)`
— six waves, six tracks each, **36 in total**, lossless CD quality. All are
genuinely stereo (side/mid −5 to −11 dB, see `check-stereo.py`), so there is
binaural information in every one.

## What is done

- **416 frequency sets imported** and committed — `data/Entrain/frequencies.set`,
  via `import-freqsets.py`. Unrelated to this, and finished.
- **Six programs measured from the earlier m4a rips of Wave I** and committed as
  `core/measured.c`. Those used the fixed-band method described below, so they
  should be **re-measured** with whatever this concludes.
- `core/progfile.c/h` — the on-disk multi-layer program format, with tests.
  Ready for the output of this work.

## The methodology, and four wrong turns

Each of these produced plausible-looking numbers, which is why they are written
down rather than deleted.

1. **Fixed carrier bands taken from Wave I.** Wave I clusters at
   60/102/114/162/300 Hz. Wave II is built on a carrier near 100.7 Hz and has a
   loud tone at 50 Hz that Wave I does not. A band list from one recording does
   not generalise, and analysing with the wrong one does not degrade — the layer
   is simply absent, silently.

2. **Discover carriers by amplitude, then pair peaks per channel.** Loud is not
   the same as binaural. Wave II's loudest tone is the ~50 Hz one and it is the
   *same in both ears*; pairing it produced a beat flipping between 0.26 Hz and
   zero. Two tones closer together than an 8 s window resolves cannot be
   measured by picking peaks at all.

3. **Phase difference at a given carrier** (`beatphase.py`). Correct maths and
   it resolves any beat, but it returns a number whether or not a pair is there.
   On that 50 Hz tone it reported a beat wandering between −0.97 and +0.72.

4. **Beat *stability* as the test** (`find-layers.py`). This is right. A real
   pair holds its beat still; nothing else does, and the gap is two orders of
   magnitude — genuine layers measure a spread (MAD) under 0.01, everything else
   sits above 0.1. Amplitude only ranks the survivors; it never decides what is
   real.

   Plus: **the band width cannot be fixed.** Wave I's pair sits 3.86 Hz apart
   and needs a wide band to contain both tones; Wave II's sits 1.5 Hz apart with
   neighbours close by and needs a narrow one. Each is invisible at the other's
   width. It is now chosen per candidate by the same steadiness criterion.

### Verified results

| | carrier | beat | steadiness (MAD) | width chosen |
| --- | --- | --- | --- | --- |
| Wave I pt 1 | 298.8 Hz | −3.852 Hz | 0.0049 | 6.0 |
| Wave II pt 1 | 100.7 Hz | −1.498 Hz | 0.0036 | 2.5 |

Both auto-selected their own width. These two are trustworthy.

## The open problem — and why the two-stage version fails

`measure-track.py` screens on excerpts then measures survivors over the whole
track, to get the run under a minute a track. **It currently finds no steady
layer on Wave I part 1**, which `find-layers.py` handles fine over a 400 s span.
Not yet diagnosed. Most likely the excerpt spans (10%, 40%, 70% of the track)
land on narration rather than tone, or concatenating three disjoint excerpts
breaks the phase unwrap across the joins — the second is the more likely of the
two and would be a real bug: `np.unwrap` across a discontinuity invents phase.

**Fix to try first: run the screen on each excerpt separately and merge the
results, never concatenating audio from different times into one analytic
signal.**

## The bigger correction, from the user

> left and right are going to have different frequencies and possibly beats
> between the two — Hemi-Sync is their name, using different carriers per hemi
> to sync with a smaller beat difference

This invalidates an assumption underneath everything above. All four methods
band-pass **one** carrier region and compare the two ears inside it, which
assumes the left and right tones sit close together around a common centre.

If the two hemispheres carry genuinely different carriers, the pairing has to be
done across channels rather than within a band:

- find the peaks in L and the peaks in R **independently**;
- consider every cross pair (L peak, R peak) whose difference is a plausible
  beat, rather than only pairs inside one band;
- test each by the same steadiness criterion, which stays valid — it is the one
  part of this that does not depend on where the tones are.

The per-channel analytic signals then have to be taken around *their own*
carriers, not around a shared centre, before the phase difference means
anything.

## Files

| file | what it is |
| --- | --- |
| `check-stereo.py` | is there binaural information at all |
| `survey-carriers.py` | where energy sits (proposes, decides nothing) |
| `analyse-binaural.py` | method 2 — superseded, kept for the record |
| `analyse-layers.py` | method 1 with fixed bands — superseded |
| `analyse-discover.py` | method 2 with discovery — superseded |
| `beatphase.py` | method 3, single carrier — useful for spot checks |
| `find-layers.py` | **method 4, works**, one track at a time, slow |
| `measure-track.py` | two-stage version — **currently broken, see above** |
| `port-measured.py` | turns measurements into program tables |
| `gen-measured.sh` | regenerates `core/measured.c` |
| `analysis/layers-*.json` | measurements from the superseded method |
