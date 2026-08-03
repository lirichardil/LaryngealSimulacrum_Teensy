# Troubleshooting Record — Cause and Solution

The six substantive problems encountered bringing this system up, each traced to
root cause. Kept separate from `DESIGN_LOG.md`, which describes the architecture
as it now stands; this file records how it got there and why.

---

## 1. Memory Pool Deadlock

**Symptom** — No pitch detection at all. Audio input dead. Not intermittent:
total, permanent failure from the first instant.

### Root cause

`AudioMemory(20)` was allocated, but `AudioAnalyzeNoteFrequency` holds **24
audio blocks simultaneously**. From `analyze_notefreq.h:40`:

```c
#define AUDIO_GUITARTUNER_BLOCKS  24
audio_block_t *blocklist1[AUDIO_GUITARTUNER_BLOCKS];
```

Its `update()` accumulates blocks into that list and only calls `release()` — in
a loop over all 24 — once the window is **completely full**
(`analyze_notefreq.cpp:65-79`).

With a 20-block pool the sequence is fatal:

```
analyzer grabs blocks → pool empties at 20 → state stalls at 20 < 24
→ release() never runs → pool stays exhausted forever
→ AudioInputI2S can't allocate → audio chain dead
```

This is a genuine **deadlock**, not degradation. There is no recovery path: the
condition that would free memory requires memory that can never be obtained.

### Solution

`AudioMemory(40)` — 24 for the analyzer plus headroom for I2S in/out and the
sine sources. (The stock `NoteFrequency` example uses 30.)

`AudioMemoryUsageMax()` is now printed every 100 ms. If it pins at the pool
size, blocks are being starved somewhere. This one diagnostic would have
identified the bug in seconds.

---

## 2. Stuck Tone

**Symptom** — After the speaker stops, the tone sustains **forever** at the last
detected pitch.

### Root cause — an API-shape trap

`AudioAnalyzeNoteFrequency` does **not** report "unvoiced." It reports *nothing
at all*.

In `process()`, `new_output = true` is set **only** when a valid period is found
(line 139). When no periodicity exists, it explicitly does the opposite
(`analyze_notefreq.cpp:147-153`):

```c
if ( tau >= HALF_BLOCKS ) {
    process_buffer  = false;
    new_output      = false;   // silence, not "unvoiced"
```

So `available()` simply stops returning true.

The original code assigned voicing state *only inside* the callback:

```c
if (notefreq.available()) {
    voiced = (freqHz >= F0_MIN_HZ && freqHz <= F0_MAX_HZ);
}
```

No callback → no assignment → `voiced` keeps its last value indefinitely.
**Absence of data was being treated as absence of change.**

### Solution

Voicing state expires on its own timer rather than waiting to be told to stop.
A silent analyzer is now interpreted as evidence, not as no-news.

---

## 3. Chopping

**Symptom** — Output breaks up mid-phrase. Worst on quiet or breathy speech.

### Root cause — five layers, one of them fundamental

| # | Cause |
|---|---|
| a | The output gate ANDed the pitch gate: `voiced = f0Valid && levelOpen`. Any YIN failure silenced the output. |
| b | **Unvoiced consonants have no periodicity.** |
| c | Strict periodicity was applied during *sustain*, but quiet and breathy phonation genuinely scores lower. |
| d | Re-entry cost the same as a cold start — one bad 70 ms frame became a 140 ms+ hole. A 3x amplifier on every marginal moment. |
| e | Decision rate is only ~14 Hz (70 ms/frame), so recovery is slow by construction. |

**Cause (b) is the fundamental one.** The sounds `s, f, t, k, p` have no
periodic structure whatsoever — YIN is *correctly* reporting failure. And they
are everywhere in connected speech. Any design that silences the output on YIN
failure will chop at **every single consonant**.

> This is the difference between a **voicing detector** and an **artificial
> larynx**. Silencing on unvoiced frames is correct behaviour for the former and
> wrong for the latter — a real larynx buzzes *continuously* through an
> utterance.

### Solution — decouple amplitude from pitch

```
Should sound come out?  ->  input LEVEL envelope only    (25 ms updates)
At what pitch?          ->  YIN only                     (70 ms updates)
                            These never gate each other.
```

YIN failure now means "hold the last pitch," never "stop making sound."

---

## 4. Ghosting

**Symptom** — Tone appears when nobody is phonating.

### Root cause

The only defence was a single RMS threshold. `probability` was read and printed
but **gated nothing**.

More importantly — and this was confirmed empirically on a disconnected input:

```
in: 0.0003  F0 tgt:150.0  prob=0.946  lvl:0  [off]
in: 0.0003  F0 tgt:150.0  prob=0.941  lvl:0  [off]
```

**YIN reported 0.94 confidence on pure noise.** It normalises its difference
function, so confidence is scale-invariant: a tiny noise floor produces the same
"certainty" as a strong voice. **Periodicity confidence alone is worthless as a
speech detector.**

### Solution — four layered gates, all applied at ONSET only

1. **60 Hz 4-pole Butterworth high-pass** — removes sub-phonation energy
   (clothing rub, cable handling, footsteps, swallowing) before it reaches
   either the level meter or the tracker.
2. **F0 range check, 80–400 Hz** — this is what actually rejected the noise
   above; the estimates were confident but out of range.
3. **`ONSET_PERIODICITY`** — an explicit confidence bar.
4. **Stability run** — N consecutive estimates agreeing within a tolerance.
   Noise pitch *wanders*; phonation *holds*. Strongest single discriminator.

**The critical design property:** none of this is re-checked once running.
Anti-ghosting costs **attack latency but never continuity** — which is exactly
why ghosting and chopping became independently tunable instead of trading
against each other.

---

## 5. Sudden Start / Stop

**Symptom** — Abrupt onset and termination. Mechanically harsh on the voice
coils.

### Root cause

The original design did the opposite of what was wanted in both directions:

- **At onset** — snapped pitch straight to target (`smoothF0 = targetF0`),
  amplitude ramp only 8 ms. The coil went from rest to a fully-formed 150 Hz
  tone in 8 ms.
- **At release** — *held* pitch at its last value indefinitely, only fading
  amplitude.

### Solution — three-regime pitch contour plus matched amplitude ramps

| Regime | Pitch target | Time constant |
|---|---|---|
| Onset | detected pitch | `F0_RISE_MS` **120 ms** — eased up from rest |
| Speaking | YIN estimate | `F0_SMOOTH_MS` **40 ms** — fast enough for intonation |
| Stopped | **0 Hz** | `F0_DECAY_MS` **250 ms** — winds down instead of holding |

Amplitude: `GATE_ATTACK_MS` 60 ms, `GATE_RELEASE_MS` 250 ms.

> **Critical coupling:** amplitude and pitch must move on comparable timescales.
> If amplitude fades in 80 ms while pitch takes 250 ms to decay, **the pitch
> decay is completely inaudible** — the sound is gone before the frequency has
> moved. These parameters must be tuned as pairs, not independently.

The one-pole smoothing also solves a second problem: YIN delivers a new number
only every 70 ms, so without it the pitch moves in audible 70 ms **steps**. A
100 → 200 Hz change now glides continuously over ~120 ms.

---

## 6. Hangover

Hangover is **both a solution and the current open problem.**

### As a solution

Bridges gaps *within* an utterance. Plosive closures (`/p/ /t/ /k/`) contain
50–100 ms of genuine silence; inter-word pauses are longer. Without hangover the
output cuts at every one.

```c
if (levelOpen) sinceLevelOk = 0;              // reset continuously while active
...
if (sinceLevelOk >= SPEECH_HANGOVER_MS)       // 250 ms
    speaking = false;
```

### As the current open problem

**Symptom** — Output sometimes persists far too long after speech stops.

**Root cause — the timer never starts.** `sinceLevelOk` is reset on *every*
cycle where `levelOpen` is true, and `levelOpen` only falls when RMS drops below
`INPUT_RMS_CLOSE` (0.012).

> If RMS never falls below 0.012, the 250 ms timer never begins counting and the
> output persists indefinitely. This is not "the hangover is too long" — it is
> "**the hangover never started**."

**Two candidate causes, not yet distinguished:**

| Cause | Mechanism |
|---|---|
| **Acoustic feedback** | Coils vibrate → conducted to throat → contact mic picks it up → level stays high → self-sustaining. Explains the intermittency: coupling varies with mounting. |
| **Ambient floor above threshold** | If room noise RMS sits between 0.012 and 0.030, `levelOpen` never clears. |

**Diagnostic** — after stopping, watch the `lvl:` field:

- `lvl:` drops to 0 promptly, sound lasts ~0.5 s → normal (250 ms hangover +
  250 ms fade)
- **`lvl:` stays 1** → one of the two causes above

**Decisive test** — disconnect the coils and repeat. If the long tail
disappears, it is feedback. This matters because **if it is feedback, no amount
of parameter tuning will fix it** — it has to be solved physically: lower
output, better mechanical isolation between coils and mic, or damping.

---

## Method notes

- Every hardware figure in this project was verified against the Teensyduino
  library source rather than taken from memory or forum posts. The 24-block
  figure in problem 1 is the clearest example: it was the difference between
  finding the bug and not.
- Diagnostics before tuning. The serial output prints each gate's state
  independently (`lvl:` / `stb:` / `prob=`) so a failure localises immediately
  instead of prompting blind parameter changes.
- Make the problem countable. The `drops:` counter turns "it sounds choppy" into
  a number: read a fixed sentence, change one parameter, compare. Target is one
  drop per sentence.
