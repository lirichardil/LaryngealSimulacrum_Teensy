#pragma once

// ===========================================================================
// Tuning.
//
// Core design decision: AMPLITUDE AND PITCH ARE DECOUPLED.
//
// Whether the output sounds is decided by the input LEVEL envelope alone.
// What pitch it sounds at is decided by YIN. The two never gate each other.
//
// This matters because YIN legitimately fails on unvoiced consonants (s, f,
// t, k, p have no periodicity at all) - which are everywhere in connected
// speech. Any design that silences the output when YIN fails will chop the
// output at every consonant. That is correct behaviour for a voicing
// detector and wrong behaviour for an artificial larynx, which should keep
// buzzing continuously through an utterance the way a real larynx does.
//
// So: YIN failing means "hold the last pitch", never "stop making sound".
// ===========================================================================

#define F0_MIN_HZ                  80.0f
#define F0_MAX_HZ                  400.0f

// Deliberately loose. This is the library's own coarse gate - it only calls
// available() when periodicity > 1 minus this value - and setting it tight
// hides marginal frames from us entirely. We want to SEE those frames and
// decide in software.
#define YIN_THRESHOLD              0.25f

// High-pass corner applied ahead of all analysis. A contact mic picks up a lot
// of sub-phonation energy - clothing rub, cable handling, footsteps, swallowing
// - which inflates the level gate. Sits below F0_MIN_HZ; at 85Hz the 4-pole
// response is only -0.3dB, so genuine low fundamentals are untouched.
#define INPUT_HP_HZ                60.0f

// --- Level gate: this and only this decides whether sound comes out --------
// OPEN starts an utterance, CLOSE ends one. The wide gap plus the hangover
// below is what carries the output through consonants and short pauses.
//
// These two are measured on DIFFERENT signals, deliberately:
//   OPEN  -> raw level (the in: field). Onset happens while we are silent, so
//            there is no feedback to reject and the full signal is wanted.
//   CLOSE -> notch-filtered level (the ntc: field), with our own output
//            frequency removed, so our own tone cannot hold the gate open.
// CALIBRATE SEPARATELY. Notching removes the voice fundamental too, so the
// notched level runs maybe 20-30% below the raw one - CLOSE needs to come
// down accordingly. OPEN must still be > CLOSE in absolute terms.
#define INPUT_RMS_OPEN             0.030f
#define INPUT_RMS_CLOSE            0.008f

// How long the output keeps sounding after the level falls away. This is the
// single most important knob for continuity: it has to outlast the silent
// closure of a plosive (/p/ /t/ /k/ are 50-100ms) and ideally short pauses
// between words too. Raise it if the output still breaks up inside a phrase;
// lower it if the tone hangs on too long after you actually stop.
#define SPEECH_HANGOVER_MS         250

// --- Acoustic feedback rejection -------------------------------------------
// The coils vibrate, that vibration conducts through tissue to the contact
// mic, the level never falls, the hangover timer never starts, and the output
// sustains forever. Confirmed on hardware: the runaway appears only once the
// coils are connected. Loop gain scales with OUTPUT_AMPLITUDE, so raising the
// output to hit the 3Vp-p coil requirement is what made it obvious.
//
// The exploitable asymmetry is that we know exactly what we are emitting: a
// PURE SINE at smoothF0, no harmonics. A real voice at the same f0 carries
// strong harmonics at 2f0, 3f0... So measuring level with f0 notched out
// leaves speech largely intact while gutting our own tone - 15-25dB of
// discrimination.
//
// Q sets notch width. Too high and the notch misses as the pitch glides
// between retunes; too low and it eats voice harmonics. 4 is ~f0/4 wide.
#define NOTCH_Q                    4.0f

// Retune the notch once smoothF0 has drifted this fraction from where the
// notch is currently centred. Recomputing coefficients costs sin/cos, so this
// avoids doing it every millisecond for no benefit.
#define NOTCH_RETUNE_RATIO         0.02f

// Hard backstop on a single continuous utterance. Nothing legitimate runs this
// long, so hitting it means the gate is stuck - feedback lock-in, or a noise
// source holding the level up. Guarantees the output always eventually stops
// instead of running indefinitely.
#define MAX_UTTERANCE_MS           8000

// --- Onset qualification: anti-ghosting, and ONLY applied at cold start ----
// To begin an utterance we require real evidence of phonation - a run of
// confident, mutually agreeing pitch estimates. Once running, none of this is
// re-checked, so it costs attack latency but never costs continuity.
#define ONSET_PERIODICITY          0.92f
#define VOICED_ONSET_COUNT         2
#define F0_AGREE_RATIO             0.15f

// Minimum periodicity to ACCEPT a pitch update while running. Lower than the
// onset bar because quiet and breathy phonation genuinely scores lower. A
// frame below this doesn't stop the output, it just doesn't move the pitch.
#define SUSTAIN_PERIODICITY        0.78f

// --- Pitch smoothing -------------------------------------------------------
// Three separate one-pole time constants, because the three situations want
// very different speeds.

// While speaking: the glide toward each new estimate. YIN delivers a new
// number only every ~70ms, so without this the pitch moves in audible 70ms
// steps. At 40ms a 100->200Hz change completes in roughly 120ms - fast enough
// to follow real intonation, slow enough to sound like a glide not a jump.
#define F0_SMOOTH_MS               40.0f

// At onset: climbing from rest up to the first detected pitch. Deliberately
// slower than tracking so the coils are eased into motion instead of being
// hit with a fully-formed tone. Reaches ~95% of target in about 3x this.
#define F0_RISE_MS                 120.0f

// After speech stops: the glide back down toward 0Hz. Wind-down rather than
// hold. Keep this in the same ballpark as GATE_RELEASE_MS - if the amplitude
// fades much faster, the pitch drop becomes inaudible and the setting does
// nothing; if much slower, the tone is gone before the pitch has moved.
#define F0_DECAY_MS                250.0f

// Averaging window for the level gate. Read RMS on this interval rather than
// every available() block: one 2.9ms block can land in a low-energy part of
// the glottal cycle and chatter the gate at the pitch rate. 25ms spans two
// full periods at F0_MIN_HZ.
#define RMS_WINDOW_MS              25

// Output amplitude (0.0-1.0 of full scale). 1.0 drives the sine to exactly
// digital full scale, which is the ceiling - AudioSynthWaveformSine clamps
// above this. Back off only if the class-D stage downstream is clipping.
#define OUTPUT_AMPLITUDE           1.0f

// Amplitude ramps, both deliberately slow so the coils are never asked to go
// from rest to full drive (or back) abruptly. Attack is paired with
// F0_RISE_MS and release with F0_DECAY_MS - amplitude and pitch should move
// on comparable timescales or one of them ends up doing nothing audible.
#define GATE_ATTACK_MS             60.0f
#define GATE_RELEASE_MS            250.0f

#define SERIAL_PRINT_INTERVAL_MS   100

// 0 = human-readable status line. 1 = "label:value" pairs for the Arduino IDE
// Serial Plotter, which graphs level and both pitch traces over time.
#define SERIAL_PLOTTER_MODE        0
#define LED_PIN                    13

#define DEFAULT_FREQ_HZ            150.0f
