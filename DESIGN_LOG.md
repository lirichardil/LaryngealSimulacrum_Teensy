# Laryngeal Simulacrum — Design Log

Engineering record for the Teensy 4.1 pitch-tracking artificial larynx driver.
Captures the reasoning behind decisions that aren't obvious from the code, plus
hardware measurements that were verified rather than assumed.

See `TROUBLESHOOTING.md` for the cause-and-solution record of each problem
encountered during bring-up.

---

## System

```
electret condenser mic (passive)
  → external preamp (~1.65 V DC bias on output)
  → series blocking cap (1 µF)
  → Audio Shield LINE IN L
       │
   Teensy 4.1 + SGTL5000
       │  detect F0, synthesise clean sine
       ↓
  Audio Shield LINE OUT L/R  (3.16 Vp-p on 1.65 V bias)
  → series blocking cap        (re-centres on 0 V)
  → class-D amplifier
  → 2 × voice coils
```

Only the **left** input channel is read; both analysis taps hang off
`audioInput` channel 0. Both output channels carry the same signal.

## Verified hardware figures

All confirmed from register comments in `control_sgtl5000.cpp` (Teensyduino
1.62.0), not from memory or forum posts.

| Quantity | Value | Source |
|---|---|---|
| LINE OUT max clean | **3.16 Vp-p** at `lineOutLevel(13)` | measured table, `control_sgtl5000.cpp:701` |
| LINE OUT DC bias | **1.65 V** (`LO_VAGCNTRL`) | reg `0x0F22`, bits 5:0 = 0x22 → 0.800 + 34×0.025 |
| LINE OUT drive current | 0.54 mA, spec'd for **10 kΩ** load | `OUT_CURRENT` already at max `0xF` |
| Input VAG | 1.575 V | `CHIP_REF_CTRL 0x01F2` |
| ADC high-pass | **active** (`ADC_HPF_BYPASS`=0) | `CHIP_ADCDAC_CTRL 0x0000` |
| DAC volume | already 0 dB | `CHIP_DAC_VOL 0x3C3C` |
| Headphone out | muted, min volume | `CHIP_ANA_CTRL 0x0036`, `HP_CTRL 0x7F7F` |

Notes worth keeping:

- `lineOutLevel` is **inverted** — lower is louder, 13 is the ceiling, and the
  function clamps anything below it. 0–12 are marked as clipping.
- 3.23 V peak against a 3.3 V rail leaves ~70 mV headroom, which independently
  confirms 13 as the true maximum.
- **LINE OUT is a voltage source, not a power source.** No register change
  gives it current; driving coils is entirely the class-D's job.
- The ADC's high-pass being active is why input DC bias does *not* corrupt the
  RMS gate — `AudioAnalyzeRMS` does no DC removal of its own.

## Architecture: why amplitude and pitch are decoupled

This took four iterations and is the core lesson.

1. **Single RMS threshold** — passed room and handling noise (ghosting), and
   dropped quiet phonation. One threshold can only trade one failure for the
   other.

2. **Uniform strict gating** — killed the ghosting but chopped the output.
   Strict periodicity was applied during *sustain*, where quiet and breathy
   phonation genuinely scores lower; and re-entry cost the same full stability
   run as a cold start, so one bad ~70 ms frame became a ~140 ms hole.

3. **Asymmetric gating** (preserved in `LaryngealSimulacrumVAD/`) — strict
   onset, permissive sustain, cheap re-entry. Much better, but the output gate
   was still `f0Valid && levelOpen`, so YIN failure still silenced the tone.

4. **Decoupled (current)** — the fix. **The level envelope alone decides
   whether sound comes out. YIN only decides the pitch.** They never gate each
   other.

The reason this is the right split: unvoiced consonants (s, f, t, k, p) have no
periodicity at all, so YIN correctly reports nothing during them, and they are
everywhere in connected speech. Any design that silences the output on YIN
failure will chop at every consonant. That is correct behaviour for a *voicing
detector* and wrong for an *artificial larynx*, which should buzz continuously
through an utterance the way a real larynx does.

Consequence: anti-ghosting lives entirely at **onset**. Once running, nothing
re-checks it, so nothing can interrupt mid-phrase. Onset knobs and continuity
knobs no longer fight each other.

### Timing

| Stage | Rate | Set by |
|---|---|---|
| YIN pitch estimate | ~70 ms | 24 blocks × 128 samples ÷ 44100 |
| RMS level | 25 ms | `RMS_WINDOW_MS` |
| Pitch glide + amp ramp | 1 ms | main loop |

The 70 ms figure is a hard floor from `AUDIO_GUITARTUNER_BLOCKS` and is why
level — not pitch — has to be the thing that gates the output.

### Pitch contour

Three one-pole time constants, selected by state:

- **onset** `F0_RISE_MS` 120 ms — climb from rest, eases the coils into motion
- **speaking** `F0_SMOOTH_MS` 40 ms — fast enough to follow real intonation
- **stopped** `F0_DECAY_MS` 250 ms — glide down to 0 Hz rather than holding

`GATE_ATTACK_MS` / `GATE_RELEASE_MS` are paired with the rise/decay constants.
If amplitude fades much faster than pitch decays, the pitch drop becomes
inaudible and the setting does nothing — they must move together.

## Open issues

1. **Occasional missed onset.** Onset is where YIN is least confident (voice
   still establishing). Candidate fix, not yet applied pending measurement:
   `ONSET_PERIODICITY` 0.92 → 0.85 and `F0_AGREE_RATIO` 0.15 → 0.25, since
   onset pitch often glides and 15 % agreement is tight there.

2. **Output sometimes persists long after speech stops.** The hangover timer
   only starts once RMS falls below `INPUT_RMS_CLOSE`; if it never does, the
   output never stops. Two candidate causes, **not yet distinguished**:
   acoustic feedback via the coils, or an ambient floor above the threshold.
   See `TROUBLESHOOTING.md` §6 for the decisive test.

## Calibration

`INPUT_RMS_OPEN` / `INPUT_RMS_CLOSE` are **starting guesses**, not measurements.
Read the `in:` field silent vs during quiet phonation and place them in the gap.

Note: raising `lineInLevel` scales signal *and* noise equally, so it improves
ADC utilisation but **not** SNR. If noise is acoustic or mechanical pickup at
the mic, gain will not touch it.

The `drops:` counter exists because choppiness is hard to judge by ear and
trivial to count — read a fixed sentence, note the number, change one
parameter, repeat. One drop per sentence is the target.

## Layout

- `LaryngealSimulacrum/` — current, active sketch.
- `LaryngealSimulacrumVAD/` — superseded. Kept as a record of the intermediate
  coupled-gating architecture; do not flash.
