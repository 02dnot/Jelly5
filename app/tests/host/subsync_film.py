#!/usr/bin/env python3
# Jelly5 — Jellyfin for PS5
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Makes the test film for tests/host/subsync.sh: "Synktest (2026).mkv", 10 min of a
# grey picture over speech-like noise bursts, with an embedded English SRT 1.7 s late
# and an external "Synktest (2026).nor.srt" 3.2 s early. Copy both into a TEST
# server's Testfiler/Synktest (2026)/ and rescan. Needs numpy and ffmpeg.
#   subsync_film.py <out dir>
import os, subprocess, sys, wave
import numpy as np

out = sys.argv[1] if len(sys.argv) > 1 else '.'
rng = np.random.default_rng(7)
sr, dur = 48000, 600.0
x = np.zeros(int(sr * dur), dtype=np.float32)
t, cues = 1.0, []
while t < dur - 5:
    length = rng.uniform(0.6, 3.0)
    n, s = int(length * sr), int(t * sr)
    burst = rng.standard_normal(n).astype(np.float32)
    burst = np.convolve(burst, np.ones(6) / 6, 'same') - np.convolve(burst, np.ones(40) / 40, 'same')
    env = np.minimum(1, np.minimum(np.arange(n), n - np.arange(n)) / (0.03 * sr))
    x[s:s + n] += 0.3 * burst * env
    cues.append((t, t + length))
    t += length + rng.uniform(0.4, 4.0)
x += 0.002 * rng.standard_normal(len(x)).astype(np.float32)
wav = os.path.join(out, 'synk-audio.wav')
with wave.open(wav, 'wb') as w:
    w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr)
    w.writeframes((np.clip(x, -1, 1) * 32000).astype('<i2').tobytes())

def srt(path, shift):
    def ts(v):
        v = max(0.0, v)
        ms = int(round(v * 1000))
        return f"{ms // 3600000:02d}:{ms // 60000 % 60:02d}:{ms // 1000 % 60:02d},{ms % 1000:03d}"
    with open(path, 'w') as f:
        for i, (a, b) in enumerate(cues):
            f.write(f"{i + 1}\n{ts(a + shift)} --> {ts(b + shift)}\nline {i + 1}\n\n")

emb = os.path.join(out, 'synk-embedded.srt')
srt(emb, 1.7)
srt(os.path.join(out, 'Synktest (2026).nor.srt'), -3.2)
subprocess.run(['ffmpeg', '-v', 'error', '-y', '-f', 'lavfi', '-i', 'color=c=gray:s=160x90:r=5:d=600', '-i', wav,
                '-i', emb, '-map', '0:v', '-map', '1:a', '-map', '2:s', '-c:v', 'libx264', '-preset', 'ultrafast',
                '-c:a', 'aac', '-b:a', '96k', '-c:s', 'srt', '-metadata:s:s:0', 'language=eng',
                os.path.join(out, 'Synktest (2026).mkv')], check=True)
os.remove(wav)
os.remove(emb)
print(len(cues), 'cues')
