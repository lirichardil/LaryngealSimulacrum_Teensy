// Teensy 4.1 + Audio Shield (SGTL5000): reads a laryngophone signal on LINE IN,
// tracks its fundamental frequency with the Audio Library's YIN-based
// AudioAnalyzeNoteFrequency, and drives LINE OUT L/R with a synthesized sine.
//
// ---------------------------------------------------------------------------
// ARCHITECTURE: amplitude and pitch are decoupled.
//
//   "Should sound come out?"  -> input LEVEL envelope, updated every 25ms.
//   "At what pitch?"          -> YIN, updated every ~70ms.
//
// These two never gate each other. YIN failing does NOT silence the output -
// it only means the pitch holds at its last value.
//
// This is the whole point. Unvoiced consonants (s, f, t, k, p) have no
// periodicity, so YIN correctly reports nothing during them, and they are
// everywhere in connected speech. The earlier design ANDed the pitch gate into
// the output gate, so it cut the tone at every consonant - correct for a
// voicing detector, wrong for an artificial larynx, which should buzz
// continuously through an utterance the way a real larynx does.
//
// Anti-ghosting still exists, but it lives entirely at ONSET: starting an
// utterance requires a run of confident agreeing pitch estimates. Once
// running, only the level envelope matters, so nothing can chop the output
// mid-phrase.
// ---------------------------------------------------------------------------
//
// Input chain: passive electret condenser mic -> external preamp -> LINE IN L.
// The preamp output carries a ~1.65V DC bias; feed it through a series
// blocking cap (1uF) so the SGTL5000 sets its own operating point at VAG
// (1.575V). Only the LEFT channel is read.
//
// Output chain: LINE OUT -> series blocking cap -> class-D amp -> voice coils.
// At OUTPUT_AMPLITUDE 1.0 and lineOutLevel(13) the output is 3.16Vp-p on a
// 1.65V DC bias; the blocking cap is what re-centres it on 0V for the amp.
#include <Audio.h>
#include <Wire.h>
#include "Config.h"

AudioInputI2S            audioInput;
AudioFilterBiquad         inputHp;
AudioFilterBiquad         notchF0;
AudioAnalyzeNoteFrequency notefreq;
AudioAnalyzeRMS           inputRms;
AudioAnalyzeRMS           notchRms;
AudioSynthWaveformSine    sineL;
AudioSynthWaveformSine    sineR;
AudioOutputI2S            audioOutput;
AudioControlSGTL5000      sgtl5000;

// All analysis reads the high-passed signal, so nothing sees sub-phonation
// rumble. Two level meters, deliberately measuring different things:
//
//   inputRms  - raw. Used for ONSET, which happens while we are silent, so
//               there is no feedback to reject and notching would only remove
//               the voice fundamental at the moment detection is hardest.
//   notchRms  - with our own output frequency notched out. Used for the STOP
//               decision, so our own tone cannot hold the gate open forever.
AudioConnection patchHp(audioInput, 0, inputHp, 0);
AudioConnection patchIn(inputHp, 0, notefreq, 0);
AudioConnection patchLevel(inputHp, 0, inputRms, 0);
AudioConnection patchNotch(inputHp, 0, notchF0, 0);
AudioConnection patchNotchLevel(notchF0, 0, notchRms, 0);
AudioConnection patchOutL(sineL, 0, audioOutput, 0);
AudioConnection patchOutR(sineR, 0, audioOutput, 1);

// --- Output gate (level-driven) --------------------------------------------
bool speaking = false;
bool prevSpeaking = false;
bool levelOpen = false;
float lastRms = 0.0f;       // raw level      - drives ONSET
float lastNotchRms = 0.0f;  // notched level  - drives STOP
float notchTunedHz = DEFAULT_FREQ_HZ;

// --- Pitch (YIN-driven, never gates the output) ----------------------------
// targetF0 is the most recent accepted estimate; smoothF0 glides toward it and
// is what actually drives the oscillators. While speaking, a YIN failure just
// leaves targetF0 where it is and the tone holds. Once speech stops, smoothF0
// is steered down to 0 instead.
float targetF0 = DEFAULT_FREQ_HZ;
float smoothF0 = 0.0f;
float lastProbability = 0.0f;

// True while climbing from rest to the first pitch of an utterance, so the
// onset can use its own slower time constant than steady-state tracking.
bool pitchRising = false;

// Onset run: consecutive confident, agreeing estimates. Only consulted while
// not already speaking.
uint8_t onsetRun = 0;
float onsetFreqHz = 0.0f;

// Current output amplitude, ramped toward its target.
float currentAmp = 0.0f;

// Utterance interruptions since boot. Continuity is hard to judge by ear and
// trivial to count: read a fixed sentence, note the figure, change one
// threshold, repeat. This should stay very low now - ideally one per sentence.
uint32_t dropouts = 0;

// Times the MAX_UTTERANCE_MS backstop had to fire. This should be ZERO in
// normal use - any nonzero value means the gate got stuck and only the
// backstop released it, which is a feedback or noise problem, not tuning.
uint32_t forcedStops = 0;

elapsedMillis sincePrint;
elapsedMillis sinceUpdate;
elapsedMillis sinceRmsRead;
elapsedMillis sinceLevelOk;
elapsedMillis sinceSpeakingStart;

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  // AudioAnalyzeNoteFrequency holds AUDIO_GUITARTUNER_BLOCKS (24) blocks at
  // once and only releases them once the window is full, so anything at or
  // below 24 deadlocks: it can never reach a full window, never releases, and
  // the pool stays exhausted. 40 leaves headroom for I2S in/out and the sines.
  AudioMemory(40);

  // enable() returns false if the SGTL5000 doesn't answer on I2C, which is the
  // one failure no amount of threshold tuning can fix.
  bool codecOk = sgtl5000.enable();
  sgtl5000.inputSelect(AUDIO_INPUT_LINEIN);
  sgtl5000.lineInLevel(5);

  // 13 = maximum clean LINE OUT, 3.16Vp-p full scale. Counterintuitively lower
  // is louder; the library clamps below 13 and the datasheet marks 0-12 as
  // clipping, so this is the chip's ceiling with no distortion penalty.
  sgtl5000.lineOutLevel(13);
  sgtl5000.unmuteLineout();

  // 4-pole Butterworth high-pass, as two cascaded biquads. The Q pair
  // (0.5412, 1.3066) is what makes the cascade maximally flat rather than
  // sagging 6dB at the corner, which two default-Q sections would do.
  inputHp.setHighpass(0, INPUT_HP_HZ, 0.54120f);
  inputHp.setHighpass(1, INPUT_HP_HZ, 1.30656f);

  // Feedback-rejection notch, retuned in the loop to track our own output.
  notchF0.setNotch(0, notchTunedHz, NOTCH_Q);

  sineL.frequency(smoothF0);
  sineL.amplitude(0.0f);
  sineR.frequency(smoothF0);
  sineR.amplitude(0.0f);

  notefreq.begin(YIN_THRESHOLD);

  // Startup blink. Proves the LED, pin 13, and a completed setup() are all
  // working independently of whether voicing is ever detected.
  //   3 slow blinks = normal.
  //   10 fast blinks = the codec didn't answer on I2C.
  int blinks = codecOk ? 3 : 10;
  int halfPeriodMs = codecOk ? 120 : 40;
  for (int i = 0; i < blinks; i++) {
    digitalWrite(LED_PIN, HIGH);
    delay(halfPeriodMs);
    digitalWrite(LED_PIN, LOW);
    delay(halfPeriodMs);
  }
  if (!codecOk) {
    Serial.println("SGTL5000 enable() failed - check audio shield seating and I2C (pins 18/19)");
  }
}

void loop() {
  // =========================================================================
  // 1. LEVEL - the only thing that decides whether sound comes out.
  // =========================================================================
  // read() averages everything accumulated since the last call, so sampling on
  // a fixed interval rather than every available() block gives a longer RMS
  // window and no chatter at the glottal-cycle rate.
  if (sinceRmsRead >= RMS_WINDOW_MS && inputRms.available() && notchRms.available()) {
    sinceRmsRead = 0;
    lastRms = inputRms.read();
    lastNotchRms = notchRms.read();

    // Retune the notch to whatever we are currently emitting. Only while
    // speaking - when silent there is no output to reject, and recomputing
    // coefficients costs sin/cos so it is not worth doing for nothing.
    if (speaking && smoothF0 >= F0_MIN_HZ &&
        fabsf(smoothF0 - notchTunedHz) > notchTunedHz * NOTCH_RETUNE_RATIO) {
      notchTunedHz = smoothF0;
      notchF0.setNotch(0, notchTunedHz, NOTCH_Q);
    }

    // Asymmetric by design, and this is the feedback fix. Opening reads the
    // RAW level: onset happens while we are silent, so the full signal is
    // wanted. Closing reads the NOTCHED level, with our own tone removed, so
    // feedback cannot hold the gate open indefinitely.
    if (lastRms >= INPUT_RMS_OPEN) levelOpen = true;
    else if (lastNotchRms < INPUT_RMS_CLOSE) levelOpen = false;

    if (levelOpen) sinceLevelOk = 0;
  }

  // =========================================================================
  // 2. PITCH - updates targetF0 only. Cannot start, stop, or interrupt sound.
  // =========================================================================
  if (notefreq.available()) {
    float freqHz = notefreq.read();
    lastProbability = notefreq.probability();
    bool inRange = (freqHz >= F0_MIN_HZ && freqHz <= F0_MAX_HZ);

    if (inRange && lastProbability >= SUSTAIN_PERIODICITY) {
      targetF0 = freqHz;
    }

    // Onset qualification. Only relevant while silent - once speaking, none of
    // this is re-checked, which is precisely why nothing can chop mid-phrase.
    //
    // levelOpen is part of the CONDITION, not just checked later at the gate.
    // That matters: without it the run accumulates during ambient noise while
    // the level gate is shut, so it sits pre-satisfied and the next level blip
    // fires instantly with no real stability evidence at all. The N agreeing
    // frames have to happen WHILE the level is up, or they prove nothing.
    if (!speaking) {
      if (levelOpen && inRange && lastProbability >= ONSET_PERIODICITY) {
        bool agrees = onsetRun > 0 &&
                      fabsf(freqHz - onsetFreqHz) <= onsetFreqHz * F0_AGREE_RATIO;
        if (agrees) {
          // Clamped. onsetRun is uint8_t and this used to increment without
          // bound, so ~255 frames of agreeing noise (about 18 seconds) wrapped
          // it back to 0 - which made the trigger behaviour change character
          // on an 18-second cycle for no visible reason.
          if (onsetRun < VOICED_ONSET_COUNT) onsetRun++;
        } else {
          onsetRun = 1;
        }
        onsetFreqHz = freqHz;
      } else {
        onsetRun = 0;
      }
    }
  }

  // =========================================================================
  // 3. UTTERANCE GATE
  // =========================================================================
  if (!speaking) {
    // Starting needs both: enough level, and real evidence of phonation.
    if (levelOpen && onsetRun >= VOICED_ONSET_COUNT) {
      speaking = true;
      sinceSpeakingStart = 0;
      // Climb from wherever the decay left us rather than snapping, so the
      // coils are eased into motion. F0_RISE_MS governs the climb.
      pitchRising = true;
    }
  } else {
    // Stopping needs sustained absence of level. The hangover is what carries
    // the tone across plosive closures and short inter-word pauses.
    //
    // MAX_UTTERANCE_MS is a backstop, not a normal exit: nothing legitimate
    // runs that long, so reaching it means the gate is stuck - feedback
    // lock-in, or a noise source holding the level up. Without it a single
    // false onset under feedback would sound forever.
    if (sinceLevelOk >= SPEECH_HANGOVER_MS || sinceSpeakingStart >= MAX_UTTERANCE_MS) {
      if (sinceSpeakingStart >= MAX_UTTERANCE_MS) forcedStops++;
      speaking = false;
      onsetRun = 0;
      levelOpen = false;   // force a genuine re-qualification, not an instant restart
    }
  }

  if (prevSpeaking && !speaking) dropouts++;
  prevSpeaking = speaking;
  digitalWrite(LED_PIN, speaking ? HIGH : LOW);

  // =========================================================================
  // 4. SYNTHESIS - pitch glide and amplitude ramp, every 1ms.
  // =========================================================================
  float dtMs = sinceUpdate;
  if (dtMs > 0) {
    sinceUpdate = 0;

    // Pitch destination: the tracked pitch while speaking, 0 once stopped.
    // Steering to 0 rather than holding is what produces the wind-down.
    float pitchTarget = speaking ? targetF0 : 0.0f;

    // Three regimes, three speeds.
    float tau;
    if (!speaking) {
      tau = F0_DECAY_MS;                  // slow glide down to rest
    } else if (pitchRising) {
      tau = F0_RISE_MS;                   // gentle climb at onset
      if (smoothF0 >= pitchTarget * 0.95f) pitchRising = false;
    } else {
      tau = F0_SMOOTH_MS;                 // fast enough to follow intonation
    }

    // One-pole glide. This is what turns YIN's 70ms staircase into a
    // continuous contour, and what makes a 100->200Hz change sound like a
    // slide instead of a jump. Written as dt/(tau+dt) rather than a fixed
    // coefficient so it stays correct if a loop iteration runs long.
    float alpha = dtMs / (tau + dtMs);
    smoothF0 += (pitchTarget - smoothF0) * alpha;

    // An exponential approach never actually arrives, so pin it at rest once
    // it is inaudibly close. Guarded on !speaking - during the onset climb
    // smoothF0 legitimately passes through this range on its way up.
    if (!speaking && smoothF0 < 1.0f) smoothF0 = 0.0f;

    // Asymmetric ramp: quick in, slow out, so the tail fades rather than cuts.
    float targetAmp = speaking ? OUTPUT_AMPLITUDE : 0.0f;
    float rampMs = (currentAmp < targetAmp) ? GATE_ATTACK_MS : GATE_RELEASE_MS;
    float step = (OUTPUT_AMPLITUDE / rampMs) * dtMs;

    if (currentAmp < targetAmp) currentAmp = min(targetAmp, currentAmp + step);
    else if (currentAmp > targetAmp) currentAmp = max(targetAmp, currentAmp - step);

    sineL.frequency(smoothF0);
    sineR.frequency(smoothF0);
    sineL.amplitude(currentAmp);
    sineR.amplitude(currentAmp);
  }

  // =========================================================================
  // 5. DIAGNOSTICS
  // =========================================================================
  // `if (Serial)` is load-bearing, not decoration. usb_serial.c blocks in
  // usb_serial_write for up to TX_TIMEOUT_MSEC (120ms) when USB is enumerated
  // but the host is not draining the buffer - i.e. powered over USB with no
  // serial monitor open. A 120ms stall here freezes the pitch glide and the
  // amplitude ramp, which is audible. Serial's operator bool() tests DTR, so
  // this skips the writes entirely unless a monitor is genuinely attached.
  // (On pure battery with no USB at all, usb_configuration is 0 and writes
  // already return immediately, so this specifically covers the USB-for-power
  // case.)
  if (Serial && sincePrint >= SERIAL_PRINT_INTERVAL_MS) {
    sincePrint = 0;
#if SERIAL_PLOTTER_MODE
    // Plotting tgt against smooth shows the glide doing its job: tgt is the
    // 70ms staircase, smooth is the contour actually being synthesised.
    Serial.print("in:");
    Serial.print(lastRms, 4);
    Serial.print("\ttgt:");
    Serial.print(targetF0, 1);
    Serial.print("\tsmooth:");
    Serial.print(smoothF0, 1);
    Serial.print("\tamp:");
    Serial.println(currentAmp, 3);
#else
    Serial.print("in: ");
    Serial.print(lastRms, 4);
    // The gap between in: and ntc: IS the feedback measurement. Our own tone
    // is a pure sine, so the notch guts it: while the coils are ringing but
    // nobody is speaking, ntc: should collapse well below in:. During real
    // speech the harmonics survive and the two stay closer together.
    Serial.print(" ntc: ");
    Serial.print(lastNotchRms, 4);
    Serial.print("  F0 tgt:");
    Serial.print(targetF0, 1);
    Serial.print(" out:");
    Serial.print(smoothF0, 1);
    Serial.print("Hz  prob=");
    Serial.print(lastProbability, 3);
    Serial.print("  lvl:");
    Serial.print(levelOpen ? 1 : 0);
    Serial.print(" stb:");
    Serial.print(onsetRun);
    Serial.print(" amp:");
    Serial.print(currentAmp, 2);
    Serial.print(speaking ? "  [ON] " : "  [off]");
    Serial.print("  drops:");
    Serial.print(dropouts);
    Serial.print(" forced:");
    Serial.print(forcedStops);
    // Restored - this was dropped in the decoupled rewrite, and it is exactly
    // the diagnostic for "works at first, misbehaves after a while". It only
    // ever rises. If it creeps toward the AudioMemory() figure over minutes,
    // blocks are being starved and the fault is in the audio graph, not here.
    Serial.print(" mem:");
    Serial.println(AudioMemoryUsageMax());
#endif
  }
}
