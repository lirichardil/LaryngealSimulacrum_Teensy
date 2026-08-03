#pragma once

// ===========================================================================
// LaryngealSimulacrumVAD - tuning
//
// SUPERSEDED. Kept as a record of the intermediate coupled-gating design.
// The active sketch is ../LaryngealSimulacrum, which decouples amplitude from
// pitch entirely. Do not flash this one.
//
// Design rule here was: STRICT TO START, PERMISSIVE TO CONTINUE, CHEAP TO
// RESUME. It fixed the choppiness of uniform strict gating, but still ANDed
// the pitch gate into the output gate, so it cut at unvoiced consonants.
// ===========================================================================

#define F0_MIN_HZ                  80.0f
#define F0_MAX_HZ                  400.0f

// Deliberately LOOSER than the plain sketch (0.15). This is the library's own
// coarse gate - it only calls available() when periodicity > 1 minus this - and
// setting it tight would hide marginal frames from us entirely. We want to SEE
// those frames and decide in software, because a frame that is too weak to
// start voicing on is often still good enough to sustain it. Raising this to
// 0.25 is what makes SUSTAIN_PERIODICITY below meaningful at all.
#define YIN_THRESHOLD              0.25f

// High-pass corner applied ahead of all analysis. A contact mic picks up a lot
// of sub-phonation energy - clothing rub, cable handling, footsteps, swallowing
// - which inflates the level gate and can hand the tracker a spurious low
// periodicity. Sits below F0_MIN_HZ; at 85Hz the 4-pole response is only
// -0.3dB, so genuine low fundamentals are untouched.
#define INPUT_HP_HZ                60.0f

// --- Level gate ------------------------------------------------------------
// OPEN is the bar for a cold start; CLOSE is the much lower bar for holding on.
// The wide gap is deliberate: it is what lets a phrase trail off quietly
// without the gate slamming shut mid-word. Calibrate OPEN clear of your
// quiet-room floor; push CLOSE as low as you can without noise sustaining.
#define INPUT_RMS_OPEN             0.030f
#define INPUT_RMS_CLOSE            0.012f

// --- Periodicity gates -----------------------------------------------------
// Asymmetric on purpose. Quiet and breathy phonation genuinely scores lower
// periodicity than full voice, so holding onset strictness through sustain is
// what chops up soft passages. ONSET keeps noise out; SUSTAIN keeps voice in.
#define ONSET_PERIODICITY          0.92f
#define SUSTAIN_PERIODICITY        0.78f

// --- Onset stability -------------------------------------------------------
// Consecutive agreeing estimates required for a COLD start. Noise produces F0
// estimates that wander; phonation produces a stable run. Each count costs one
// analysis frame (~70ms) of attack latency.
#define VOICED_ONSET_COUNT         2
#define F0_AGREE_RATIO             0.15f

// --- Re-arm window ---------------------------------------------------------
// The fix for choppiness. Within this long after the last voiced moment, a
// restart is treated as the same utterance resuming rather than a fresh onset:
// one acceptable frame re-enters, no stability run, sustain-level thresholds.
// Without this, every momentary dropout costs the full VOICED_ONSET_COUNT
// again, which is what turns one bad frame into an audible ~140ms hole.
#define REARM_MS                   350

// How long voicing survives with no acceptable estimate. Longer than the plain
// sketch (150ms) so a single dropped frame rides through instead of killing
// the note - at ~70ms per frame this tolerates two consecutive misses.
#define F0_TIMEOUT_MS              250

// Averaging window for the level gate.
#define RMS_WINDOW_MS              25

#define OUTPUT_AMPLITUDE           0.3f
#define GATE_RAMP_MS               8.0f

#define SERIAL_PRINT_INTERVAL_MS   100
#define SERIAL_PLOTTER_MODE        0
#define LED_PIN                    13

#define DEFAULT_FREQ_HZ            150.0f
