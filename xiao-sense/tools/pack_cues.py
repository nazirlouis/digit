"""Pack authored 16-kHz WAV cues as IMA ADPCM; leave source WAVs intact."""
from pathlib import Path
import argparse
import struct
import wave

STEPS = (
    7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,
    73,80,88,97,107,118,130,143,157,173,190,209,230,253,279,307,337,371,
    408,449,494,544,598,658,724,796,876,963,1060,1166,1282,1411,1552,
    1707,1878,2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,
    5894,6484,7132,7845,8630,9493,10442,11487,12635,13899,15289,16818,
    18500,20350,22385,24623,27086,29794,32767,
)
CHANGES = (-1,-1,-1,-1,2,4,6,8)

def encode_samples(pcm, predictor=0, index=0):
    packed = bytearray()
    codes = []
    for (sample,) in struct.iter_unpack('<h', pcm):
        step = STEPS[index]
        difference = sample - predictor
        code = 8 if difference < 0 else 0
        difference = abs(difference)
        delta = step >> 3
        for bit, amount in ((4, step), (2, step >> 1), (1, step >> 2)):
            if difference >= amount:
                code |= bit
                difference -= amount
                delta += amount
        predictor = max(-32768, min(32767, predictor + (-delta if code & 8 else delta)))
        index = max(0, min(88, index + CHANGES[code & 7]))
        codes.append(code)
        if len(codes) == 2:
            packed.append((codes[0] << 4) | codes[1])
            codes.clear()
    if codes:
        packed.append(codes[0] << 4)
    return packed

def encode(pcm):
    samples = [value for (value,) in struct.iter_unpack('<h', pcm)]
    packed = bytearray(struct.pack('<I', len(samples)))
    for at in range(0, len(samples), 64):
        block = samples[at:at + 64]
        desired = max((abs(b-a) for a,b in zip(block, block[1:])), default=0) // 2
        index = next((i for i, step in enumerate(STEPS) if step >= desired), 88)
        packed.extend(struct.pack('<hBB', block[0], index, 0))
        tail = struct.pack('<%dh' % (len(block) - 1), *block[1:])
        packed.extend(encode_samples(tail, block[0], index))
    return packed

def pack(source, destination):
    destination.mkdir(parents=True, exist_ok=True)
    for path in sorted(source.glob('*.wav')):
        with wave.open(str(path), 'rb') as wav:
            assert (wav.getnchannels(), wav.getsampwidth(), wav.getframerate()) == (1, 2, 16000), path
            result = encode(wav.readframes(wav.getnframes()))
        target = destination / (path.stem + '.adpcm')
        if not target.exists() or target.read_bytes() != result:
            target.write_bytes(result)

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('source', type=Path)
    parser.add_argument('destination', type=Path)
    args = parser.parse_args()
    pack(args.source, args.destination)
