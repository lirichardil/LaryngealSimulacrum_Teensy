// SUPERSEDED - kept as a record of the intermediate design. The active sketch
// is ../LaryngealSimulacrum. Do not flash this one.
//
// Teensy 4.1 + Audio Shield (SGTL5000) - noise-gating variant. Same job: track
// the fundamental of a laryngophone signal on LINE IN and drive LINE OUT L/R
// with a clean sine at that pitch. The difference is entirely in WHEN it
// decides phonation is happening.
//
// The plain sketch used one RMS threshold, which passes room and handling
// noise. A first attempt at fixing that applied strict gates uniformly, and
// broke continuity: quiet or breathy phonation scores lower periodicity, so it
// got rejected mid-phrase, and because re-entry cost the same full stability
// run as a cold start, every dropped frame became an audible ~140ms hole.
//
// So the gating here is asymmetric in three separate ways:
//
//   onset   - strict. High level bar, high periodicity bar, and a run of
//             agreeing estimates. Costs latency; buys noise rejection.
//   sustain - slack. Low level bar, low periodicity bar, single frame, and a
//             longer timeout, so soft tails keep tracking.
//   re-arm  - within REARM_MS of the last voiced moment, a restart skips the
//             stability run entirely and uses sustain thresholds.
//
// WHY THIS WAS STILL NOT ENOUGH: the output gate is `f0Valid && levelOpen`, so
// a YIN failure still silences the tone. Unvoiced consonants have no
// periodicity at all, so this still chops at every s/f/t/k/p. The successor
// sketch fixes that by decoupling amplitude from pitch entirely.
#include <Audio.h>
#include <Wire.h>
#include "Config.h"

AudioInputI2S            audioInput;
AudioFilterBiquad         inputHp;
AudioAnalyzeNoteFrequency notefreq;
AudioAnalyzeRMS           inputRms;
AudioSynthWaveformSine    sineL;
AudioSynthWaveformSine    sineR;
AudioOutputI2S            audioOutput;
AudioControlSGTL5000      sgtl5000;

AudioConnection patchHp(audioInput, 0, inputHp, 0);
AudioConnection patchIn(inputHp, 0, notefreq, 0);
AudioConnection patchLevel(inputHp, 0, inputRms, 0);
AudioConnection patchOutL(sineL, 0, audioOutput, 0);
AudioConnection patchOutR(sineR, 0, audioOutput, 1);


struct ChannelState {
  float targetFreqHz = DEFAULT_FREQ_HZ;
  float targetAmp = 0.0f;
  float currentAmp = 0.0f;
};
ChannelState chanL, chanR;

bool voiced = false;
bool prevVoiced = false;
bool f0Valid = false;
bool levelOpen = false;
float lastFreqHz = 0.0f;
float lastProbability = 0.0f;
float lastRms = 0.0f;

uint8_t onsetRun = 0;
float onsetFreqHz = 0.0f;
uint32_t dropouts = 0;

elapsedMillis sincePrint;
elapsedMillis sinceRampUpdate;
elapsedMillis sinceF0Update;
elapsedMillis sinceRmsRead;
elapsedMillis sinceVoiced;

void setChannelPitch(ChannelState& chan, float freqHz, bool isVoiced) {
  if (isVoiced) {
    chan.targetFreqHz = freqHz;
    chan.targetAmp = OUTPUT_AMPLITUDE;
  } else {
    chan.targetAmp = 0.0f;
  }
}

void applyChannel(AudioSynthWaveformSine& synth, ChannelState& chan, float dtMs) {
  synth.frequency(chan.targetFreqHz);

  float step = (OUTPUT_AMPLITUDE / GATE_RAMP_MS) * dtMs;
  if (chan.currentAmp < chan.targetAmp) {
    chan.currentAmp = min(chan.targetAmp, chan.currentAmp + step);
  } else if (chan.currentAmp > chan.targetAmp) {
    chan.currentAmp = max(chan.targetAmp, chan.currentAmp - step);
  }
  synth.amplitude(chan.currentAmp);
}

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  AudioMemory(40);

  sgtl5000.enable();
  sgtl5000.inputSelect(AUDIO_INPUT_LINEIN);
  sgtl5000.lineInLevel(5);
  sgtl5000.lineOutLevel(29);
  sgtl5000.unmuteLineout();

  inputHp.setHighpass(0, INPUT_HP_HZ, 0.54120f);
  inputHp.setHighpass(1, INPUT_HP_HZ, 1.30656f);

  sineL.frequency(chanL.targetFreqHz);
  sineL.amplitude(0.0f);
  sineR.frequency(chanR.targetFreqHz);
  sineR.amplitude(0.0f);

  notefreq.begin(YIN_THRESHOLD);
}

void loop() {
  if (sinceRmsRead >= RMS_WINDOW_MS && inputRms.available()) {
    sinceRmsRead = 0;
    lastRms = inputRms.read();

    if (lastRms >= INPUT_RMS_OPEN) levelOpen = true;
    else if (lastRms < INPUT_RMS_CLOSE) levelOpen = false;
  }

  bool warm = (sinceVoiced < REARM_MS);

  if (notefreq.available()) {
    float freqHz = notefreq.read();
    lastProbability = notefreq.probability();
    bool inRange = (freqHz >= F0_MIN_HZ && freqHz <= F0_MAX_HZ);

    if (f0Valid) {
      if (inRange && lastProbability >= SUSTAIN_PERIODICITY) {
        lastFreqHz = freqHz;
        sinceF0Update = 0;
      }
    } else if (warm) {
      if (inRange && lastProbability >= SUSTAIN_PERIODICITY) {
        lastFreqHz = freqHz;
        sinceF0Update = 0;
        f0Valid = true;
        onsetRun = VOICED_ONSET_COUNT;
      }
    } else {
      if (inRange && lastProbability >= ONSET_PERIODICITY) {
        bool agrees = onsetRun > 0 &&
                      fabsf(freqHz - onsetFreqHz) <= onsetFreqHz * F0_AGREE_RATIO;
        onsetRun = agrees ? onsetRun + 1 : 1;
        onsetFreqHz = freqHz;

        if (onsetRun >= VOICED_ONSET_COUNT) {
          lastFreqHz = freqHz;
          sinceF0Update = 0;
          f0Valid = true;
        }
      } else {
        onsetRun = 0;
      }
    }
  }

  if (f0Valid && sinceF0Update >= F0_TIMEOUT_MS) {
    f0Valid = false;
    onsetRun = 0;
    lastProbability = 0.0f;
  }

  voiced = f0Valid && levelOpen;
  if (voiced) sinceVoiced = 0;
  if (prevVoiced && !voiced) dropouts++;
  prevVoiced = voiced;

  setChannelPitch(chanL, lastFreqHz, voiced);
  setChannelPitch(chanR, lastFreqHz, voiced);

  digitalWrite(LED_PIN, voiced ? HIGH : LOW);

  float dtMs = sinceRampUpdate;
  if (dtMs > 0) {
    sinceRampUpdate = 0;
    applyChannel(sineL, chanL, dtMs);
    applyChannel(sineR, chanR, dtMs);
  }

  if (sincePrint >= SERIAL_PRINT_INTERVAL_MS) {
    sincePrint = 0;
#if SERIAL_PLOTTER_MODE
    Serial.print("in:");
    Serial.print(lastRms, 4);
    Serial.print("\tf0:");
    Serial.print(voiced ? lastFreqHz : 0.0f, 1);
    Serial.print("\topen:");
    Serial.print(INPUT_RMS_OPEN, 4);
    Serial.print("\tclose:");
    Serial.println(INPUT_RMS_CLOSE, 4);
#else
    Serial.print("in: ");
    Serial.print(lastRms, 4);
    Serial.print("  F0: ");
    if (voiced) {
      Serial.print(lastFreqHz, 1);
      Serial.print(" Hz");
    } else {
      Serial.print("---   ");
    }
    Serial.print("  prob=");
    Serial.print(lastProbability, 3);
    Serial.print("  lvl:");
    Serial.print(levelOpen ? 1 : 0);
    Serial.print(" stb:");
    Serial.print(onsetRun);
    Serial.print(" warm:");
    Serial.print(warm ? 1 : 0);
    Serial.print(voiced ? "  [voiced]" : "  [unvoiced]");
    Serial.print("  drops:");
    Serial.println(dropouts);
#endif
  }
}
