#!/usr/bin/env python3
"""Makes Inertia's sound effects.

The sounds are synthesized here with a port of the classic sfxr generator
(square, sawtooth, sine and noise waves shaped by an envelope, pitch slides,
vibrato, a pitch jump, a phaser and two filters), then written out twice:

  ../sounds.h      8-bit samples compiled into the sketch
  preview/*.wav    the same samples as WAV files, to listen to on a computer

Run it again after changing a recipe below:

    python3 Inertia/tools/make_sounds.py

It needs nothing beyond Python itself. The sounds are the same on every run.

sfxr is by Tomas Pettersson (DrPetter), copyright (c) 2007, and is released
under the MIT license: https://www.drpetter.se/project_sfxr.html
"""

import math
import os
import random
import struct
import wave

SYNTH_RATE = 44100   # sfxr's own rate; its settings are tuned to it
OUT_RATE = 22050     # what the sketch plays
HERE = os.path.dirname(os.path.abspath(__file__))

SQUARE, SAW, SINE, NOISE = range(4)

# sfxr's settings, all 0..1 (or -1..1 where a direction makes sense).
DEFAULTS = dict(
    wave=SQUARE,
    base_freq=0.3, freq_limit=0.0, freq_ramp=0.0, freq_dramp=0.0,
    duty=0.0, duty_ramp=0.0,
    vib_strength=0.0, vib_speed=0.0,
    env_attack=0.0, env_sustain=0.3, env_decay=0.4, env_punch=0.0,
    lpf_freq=1.0, lpf_ramp=0.0, lpf_resonance=0.0,
    hpf_freq=0.0, hpf_ramp=0.0,
    pha_offset=0.0, pha_ramp=0.0,
    repeat_speed=0.0,
    arp_speed=0.0, arp_mod=0.0,
)


# ---- Friendlier units for the settings ------------------------------------
def hz(frequency):
    """base_freq for a starting pitch in Hz."""
    return math.sqrt(max(100.0 * frequency / (SYNTH_RATE * 8) - 0.001, 0.0))


def secs(seconds):
    """An envelope setting (attack, sustain or decay) for a length in seconds."""
    return math.sqrt(seconds * SYNTH_RATE / 100000.0)


def jump(ratio):
    """arp_mod for a pitch jump: 1.5 jumps up a fifth, 0.5 drops an octave."""
    if ratio >= 1:
        return math.sqrt((1 - 1 / ratio) / 0.9)
    return -math.sqrt((1 / ratio - 1) / 10.0)


def jump_after(seconds):
    """arp_speed that makes the pitch jump happen this long into the sound."""
    return 1 - math.sqrt(max(seconds * SYNTH_RATE - 32, 0) / 20000.0)


def repeat_every(seconds):
    """repeat_speed that restarts the pitch slide this often."""
    return 1 - math.sqrt(max(seconds * SYNTH_RATE - 32, 0) / 20000.0)


def vibrato_hz(frequency):
    """vib_speed for a wobble this many times a second."""
    return math.sqrt(frequency * 2 * math.pi / SYNTH_RATE / 0.01)


# ---- The synthesizer (a port of sfxr's SynthSample) ------------------------
def synth(seed=1, **settings):
    """Returns one sound as a list of floats at SYNTH_RATE."""
    unknown = set(settings) - set(DEFAULTS)
    if unknown:
        raise ValueError("unknown settings: %s" % ", ".join(sorted(unknown)))
    p = dict(DEFAULTS)
    p.update(settings)
    rng = random.Random(seed)

    s = {}

    def restart():
        # The part of the set-up that a "repeat" does over again.
        s["fperiod"] = 100.0 / (p["base_freq"] ** 2 + 0.001)
        s["fmaxperiod"] = 100.0 / (p["freq_limit"] ** 2 + 0.001)
        s["fslide"] = 1.0 - p["freq_ramp"] ** 3 * 0.01
        s["fdslide"] = -(p["freq_dramp"] ** 3) * 0.000001
        s["duty"] = 0.5 - p["duty"] * 0.5
        s["duty_slide"] = -p["duty_ramp"] * 0.00005
        if p["arp_mod"] >= 0:
            s["arp_mod"] = 1.0 - p["arp_mod"] ** 2 * 0.9
        else:
            s["arp_mod"] = 1.0 + p["arp_mod"] ** 2 * 10.0
        s["arp_time"] = 0
        s["arp_limit"] = int((1.0 - p["arp_speed"]) ** 2 * 20000 + 32)
        if p["arp_speed"] == 0.0 or p["arp_speed"] == 1.0:
            s["arp_limit"] = 0

    restart()
    phase = 0
    fltp = fltdp = fltphp = 0.0
    fltw = p["lpf_freq"] ** 3 * 0.1
    fltw_d = 1.0 + p["lpf_ramp"] * 0.0001
    fltdmp = min(5.0 / (1.0 + p["lpf_resonance"] ** 2 * 20.0) * (0.01 + fltw), 0.8)
    flthp = p["hpf_freq"] ** 2 * 0.1
    flthp_d = 1.0 + p["hpf_ramp"] * 0.0003
    vib_phase = 0.0
    vib_speed = p["vib_speed"] ** 2 * 0.01
    vib_amp = p["vib_strength"] * 0.5
    env_length = [int(p["env_attack"] ** 2 * 100000),
                  int(p["env_sustain"] ** 2 * 100000),
                  int(p["env_decay"] ** 2 * 100000)]
    env_stage = 0
    env_time = 0
    fphase = math.copysign(p["pha_offset"] ** 2 * 1020.0, p["pha_offset"])
    fdphase = math.copysign(p["pha_ramp"] ** 2, p["pha_ramp"])
    phaser = [0.0] * 1024
    ipp = 0
    noise = [rng.uniform(-1, 1) for _ in range(32)]
    rep_time = 0
    rep_limit = int((1.0 - p["repeat_speed"]) ** 2 * 20000 + 32) if p["repeat_speed"] else 0
    use_lpf = p["lpf_freq"] != 1.0
    wave_type = p["wave"]

    out = []
    while True:
        rep_time += 1
        if rep_limit and rep_time >= rep_limit:
            rep_time = 0
            restart()

        # Pitch: the jump, the slide, then vibrato on top.
        s["arp_time"] += 1
        if s["arp_limit"] and s["arp_time"] >= s["arp_limit"]:
            s["arp_limit"] = 0
            s["fperiod"] *= s["arp_mod"]
        s["fslide"] += s["fdslide"]
        s["fperiod"] *= s["fslide"]
        if s["fperiod"] > s["fmaxperiod"]:
            s["fperiod"] = s["fmaxperiod"]
            if p["freq_limit"] > 0:
                break
        rfperiod = s["fperiod"]
        if vib_amp > 0:
            vib_phase += vib_speed
            rfperiod *= 1.0 + math.sin(vib_phase) * vib_amp
        period = max(int(rfperiod), 8)
        s["duty"] = min(max(s["duty"] + s["duty_slide"], 0.0), 0.5)
        duty = s["duty"]

        # Loudness envelope: attack, sustain (with an optional punch), decay.
        env_time += 1
        while env_stage < 3 and (env_time > env_length[env_stage] or env_length[env_stage] == 0):
            env_time = 0
            env_stage += 1
        if env_stage == 3:
            break
        if env_stage == 0:
            env_vol = env_time / env_length[0]
        elif env_stage == 1:
            env_vol = 1.0 + (1.0 - env_time / env_length[1]) * 2.0 * p["env_punch"]
        else:
            env_vol = 1.0 - env_time / env_length[2]

        fphase += fdphase
        iphase = min(abs(int(fphase)), 1023)
        if flthp_d != 0.0:
            flthp = min(max(flthp * flthp_d, 0.00001), 0.1)

        total = 0.0
        for _ in range(8):  # 8 fine steps per output sample, averaged
            phase += 1
            if phase >= period:
                phase %= period
                if wave_type == NOISE:
                    noise = [rng.uniform(-1, 1) for _ in range(32)]
            fp = phase / period
            if wave_type == SQUARE:
                sample = 0.5 if fp < duty else -0.5
            elif wave_type == SAW:
                sample = 1.0 - fp * 2
            elif wave_type == SINE:
                sample = math.sin(fp * 2 * math.pi)
            else:
                sample = noise[phase * 32 // period]
            # Low-pass, then high-pass.
            pp = fltp
            fltw = min(max(fltw * fltw_d, 0.0), 0.1)
            if use_lpf:
                fltdp += (sample - fltp) * fltw
                fltdp -= fltdp * fltdmp
            else:
                fltp = sample
                fltdp = 0.0
            fltp += fltdp
            fltphp += fltp - pp
            fltphp -= fltphp * flthp
            sample = fltphp
            # Phaser: the sound added to a slightly delayed copy of itself.
            phaser[ipp & 1023] = sample
            sample += phaser[(ipp - iphase + 1024) & 1023]
            ipp = (ipp + 1) & 1023
            total += sample * env_vol
        out.append(total / 8)
    return out


# ---- Putting sounds together ------------------------------------------------
def mix(*parts):
    """Layers sounds. Each part is (start in seconds, samples, loudness)."""
    length = max(int(start * SYNTH_RATE) + len(samples) for start, samples, _ in parts)
    out = [0.0] * length
    for start, samples, gain in parts:
        peak = max(abs(v) for v in samples) or 1.0
        offset = int(start * SYNTH_RATE)
        for i, v in enumerate(samples):
            out[offset + i] += v / peak * gain
    return out


def loop_mix(step, notes):
    """Lays notes out `step` seconds apart as a seamless loop: a note still
    ringing at the end carries on over the start."""
    spacing = int(step * SYNTH_RATE)
    out = [0.0] * (spacing * len(notes))
    for n, samples in enumerate(notes):
        for i, v in enumerate(samples):
            out[(n * spacing + i) % len(out)] += v
    return out


def finish(samples, loudness=1.0):
    """Halves the rate to OUT_RATE and scales to 8-bit, -127..127."""
    halved = [(samples[i] + samples[i + 1]) / 2 for i in range(0, len(samples) - 1, 2)]
    peak = max(abs(v) for v in halved) or 1.0
    return [int(round(v / peak * loudness * 127)) for v in halved]


# ---- The recipes ------------------------------------------------------------
def star(pitch):
    # The classic coin: a bright square note that jumps up a fourth.
    return synth(wave=SQUARE, base_freq=hz(pitch), duty=0.35,
                 env_sustain=secs(0.05), env_decay=secs(0.30), env_punch=0.5,
                 arp_mod=jump(4 / 3), arp_speed=jump_after(0.065))


def warning():
    # Two falling "uh-oh" swoops: a weight is on its way.
    return synth(wave=SAW, base_freq=hz(520), freq_ramp=-0.33, duty=0.2,
                 repeat_speed=repeat_every(0.17), vib_strength=0.12, vib_speed=vibrato_hz(30),
                 env_sustain=secs(0.26), env_decay=secs(0.12), lpf_freq=0.75)


def build():
    sounds = {}

    # Stars climb a major scale toward the next weight.
    star_pitches = [988, 1109, 1245, 1319]  # B5, C#6, D#6, E6
    for i, pitch in enumerate(star_pitches):
        sounds["STAR_%d" % i] = finish(star(pitch), 0.80)
    # The star that brings a weight: the top chime, then the warning under it.
    sounds["STAR_WEIGHT"] = finish(mix((0.0, star(star_pitches[-1]), 0.85),
                                       (0.20, warning(), 1.0)), 0.90)

    # The puck on an edge: a tiny plucked click that drops in pitch.
    sounds["TICK"] = finish(synth(wave=SQUARE, base_freq=hz(1500), freq_ramp=-0.42, duty=0.4,
                                  env_sustain=secs(0.008), env_decay=secs(0.05),
                                  hpf_freq=0.15), 0.85)

    # A weight on an edge or another weight: a short, dull thump of noise.
    sounds["THUD"] = finish(synth(wave=NOISE, seed=7, base_freq=hz(260), freq_ramp=-0.30,
                                  env_sustain=secs(0.015), env_decay=secs(0.13), env_punch=0.6,
                                  lpf_freq=0.55, lpf_ramp=-0.3), 1.0)

    # The saucer's sound, looped while it is on screen: a friendly music-box
    # twinkle, soft bell notes running up and down a major chord.
    def bell(pitch):
        return synth(wave=SINE, base_freq=hz(pitch), env_attack=secs(0.004),
                     env_sustain=secs(0.012), env_decay=secs(0.16), env_punch=0.2)
    twinkle = [1047, 1319, 1568, 2093, 1568, 1319]  # C6 E6 G6 C7 G6 E6
    sounds["SAUCER_LOOP"] = finish(loop_mix(0.095, [bell(pitch) for pitch in twinkle]), 0.60)

    # Catching the saucer: the classic power-up, a rising note that restarts
    # higher and higher.
    sounds["CAUGHT"] = finish(synth(wave=SQUARE, base_freq=hz(330), freq_ramp=0.22, duty=0.6,
                                    duty_ramp=-0.3, repeat_speed=repeat_every(0.085),
                                    env_sustain=secs(0.30), env_decay=secs(0.40),
                                    pha_offset=0.15, pha_ramp=-0.1), 0.85)

    # The end of a run: a crunchy explosion over a note sliding down and away.
    boom = synth(wave=NOISE, seed=3, base_freq=hz(900), freq_ramp=-0.22,
                 env_sustain=secs(0.12), env_decay=secs(0.65), env_punch=0.55,
                 pha_offset=-0.2, pha_ramp=-0.15, lpf_freq=0.85, lpf_ramp=-0.2)
    fall = synth(wave=SQUARE, base_freq=hz(740), freq_ramp=-0.20, freq_dramp=-0.15, duty=0.3,
                 vib_strength=0.2, vib_speed=vibrato_hz(14),
                 env_sustain=secs(0.25), env_decay=secs(0.50))
    crash = mix((0.0, boom, 1.0), (0.03, fall, 0.55))
    sounds["GAME_OVER"] = finish(crash, 1.0)

    # The end of a run that set a new best: the same crash, a short
    # silence (the crash lasts about 0.8 seconds), then a little fanfare. Three quick rising notes and a long, shimmering top note with
    # a harmony above it.
    def horn(pitch, length, duty=0.5, shimmer=0.0):
        return synth(wave=SQUARE, base_freq=hz(pitch), duty=duty, duty_ramp=0.15,
                     vib_strength=shimmer, vib_speed=vibrato_hz(7),
                     env_attack=secs(0.004), env_sustain=secs(length * 0.6),
                     env_decay=secs(length * 0.4), env_punch=0.3)
    fanfare = mix((0.00, horn(523, 0.11), 0.8),              # C5
                  (0.11, horn(659, 0.11), 0.8),              # E5
                  (0.22, horn(784, 0.11), 0.8),              # G5
                  (0.33, horn(1047, 0.60, shimmer=0.06), 1.0),         # C6, held
                  (0.33, horn(1319, 0.60, duty=0.25, shimmer=0.06), 0.45),  # E6 above it
                  (0.45, horn(1568, 0.48, duty=0.25, shimmer=0.06), 0.35))  # G6 joining in
    sounds["GAME_OVER_BEST"] = finish(mix((0.0, crash, 0.9), (1.00, fanfare, 1.0)), 1.0)

    # The start of a run: three quick rising blips, the last one jumping up.
    def blip(pitch, last=False):
        return synth(wave=SQUARE, base_freq=hz(pitch), duty=0.5, duty_ramp=0.4,
                     env_sustain=secs(0.04), env_decay=secs(0.22 if last else 0.09),
                     env_punch=0.3,
                     arp_mod=jump(2.0) if last else 0.0,
                     arp_speed=jump_after(0.07) if last else 0.0)
    sounds["START"] = finish(mix((0.00, blip(523), 0.8), (0.09, blip(659), 0.8),
                                 (0.18, blip(784, last=True), 1.0)), 0.80)

    # Sound switched back on: one small rising blip.
    sounds["UNMUTE"] = finish(synth(wave=SQUARE, base_freq=hz(880), duty=0.5,
                                    env_sustain=secs(0.03), env_decay=secs(0.10),
                                    arp_mod=jump(1.5), arp_speed=jump_after(0.04)), 0.70)
    return sounds


# ---- Output -------------------------------------------------------------------
def write_header(sounds, path):
    lines = [
        "// Inertia's sound effects: 8-bit samples at %d per second." % OUT_RATE,
        "// Generated by tools/make_sounds.py. Don't edit by hand; change the",
        "// recipes there and run it again.",
        "#pragma once",
        "#include <stddef.h>",
        "#include <stdint.h>",
        "",
        "struct Sound {",
        "  const int8_t* samples;",
        "  size_t count;",
        "};",
        "static const uint32_t SOUND_RATE = %d;" % OUT_RATE,
        "",
    ]
    for name, samples in sounds.items():
        lines.append("static const int8_t SND_%s_SAMPLES[] = {" % name)
        for i in range(0, len(samples), 24):
            lines.append("  " + ",".join(str(v) for v in samples[i:i + 24]) + ",")
        lines.append("};")
        lines.append("static const Sound SND_%s = {SND_%s_SAMPLES, sizeof(SND_%s_SAMPLES)};"
                     % (name, name, name))
        lines.append("")
    with open(path, "w") as f:
        f.write("\n".join(lines))


def write_wav(samples, path):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(1)  # 8-bit WAV is unsigned
        w.setframerate(OUT_RATE)
        w.writeframes(struct.pack("%dB" % len(samples), *[v + 128 for v in samples]))


def main():
    sounds = build()
    write_header(sounds, os.path.join(HERE, "..", "sounds.h"))
    preview = os.path.join(HERE, "preview")
    os.makedirs(preview, exist_ok=True)
    total = 0
    for name, samples in sounds.items():
        write_wav(samples, os.path.join(preview, name.lower() + ".wav"))
        total += len(samples)
        print("%-12s %5.2f s  %6d bytes" % (name, len(samples) / OUT_RATE, len(samples)))
    print("%-12s %5.2f s  %6d bytes" % ("total", total / OUT_RATE, total))


if __name__ == "__main__":
    main()
