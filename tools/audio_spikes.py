"""Find single-sample spikes that exist in one channel only (Riven).

usage: python tools/audio_spikes.py build/hw/pcm.raw [rate=48000]
Input: int16 interleaved stereo (kirby.exe --audio-raw). Reports, per channel, spikes with no
counterpart in the other channel, their phase inside the 59.7275 Hz frame and their height.
"""
import array, collections, sys

path = sys.argv[1]
sr = int(sys.argv[2]) if len(sys.argv) > 2 else 48000
a = array.array('h'); a.frombytes(open(path, 'rb').read())
ch = {'L': a[0::2], 'R': a[1::2]}
n = len(ch['L'])
THR = 9000
FRAME = 280896 / 16777216            # seconds per GBA frame
print(f'{n} frames, {n / sr:.1f} s')
for name, other in (('L', 'R'), ('R', 'L')):
    x, y = ch[name], ch[other]
    big = [i for i in range(1, n) if abs(x[i] - x[i - 1]) > THR]
    near = set(i for i in range(1, n) if abs(y[i] - y[i - 1]) > THR // 3)
    only = [i for i in big if not any((i + k) in near for k in range(-6, 7))]
    ev = []
    for i in only:
        if not ev or i - ev[-1] > 20:
            ev.append(i)
    ph = collections.Counter(int(((i / sr) % FRAME) / FRAME * 10) for i in ev)
    hts = collections.Counter((x[i + 1] - x[i - 2]) // 256 for i in ev)
    print(f'{name}-only spikes: {len(ev)}  per frame-phase decile: {[ph.get(k, 0) for k in range(10)]}')
    print(f'   height in PCM units (top 5): {hts.most_common(5)}')
