"""Check the firmware cue encoder against Python's independent IMA codec."""
from pathlib import Path
import audioop
import importlib.util
import struct
import unittest
import wave

PROJECT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('pack_cues', PROJECT / 'tools' / 'pack_cues.py')
packer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(packer)

class CuePackingTests(unittest.TestCase):
    def test_all_spoken_cues_match_reference_and_keep_duration(self):
        output = PROJECT / 'test-artifacts' / 'cues'
        output.mkdir(parents=True, exist_ok=True)
        total_original = total_packed = 0
        paths = list((PROJECT / 'audio_cues').glob('*.wav'))
        self.assertEqual(len(paths), 9)
        for path in paths:
            with self.subTest(cue=path.stem), wave.open(str(path), 'rb') as wav:
                self.assertEqual((wav.getnchannels(), wav.getsampwidth(), wav.getframerate()), (1, 2, 16000))
                samples = wav.getnframes()
                pcm = wav.readframes(samples)
                packed = packer.encode(pcm)
                self.assertEqual(struct.unpack('<I', packed[:4])[0], samples)
                decoded = bytearray()
                for at in range(0, samples, 64):
                    count = min(64, samples - at)
                    offset = 4 + (at // 64) * 36
                    predictor, index, _ = struct.unpack('<hBB', packed[offset:offset + 4])
                    encoded = packed[offset + 4:offset + 4 + count // 2]
                    source = pcm[(at + 1)*2:(at + count)*2]
                    # Add one dummy sample so audioop returns the final high nibble.
                    reference, _ = audioop.lin2adpcm(source + (b'\0\0' if len(source) % 4 else b''), 2, (predictor, index))
                    if len(source) % 4:
                        reference = reference[:-1] + bytes([reference[-1] & 0xf0])
                    self.assertEqual(encoded, reference)
                    recovered, _ = audioop.adpcm2lin(encoded, 2, (predictor, index))
                    decoded.extend(struct.pack('<h', predictor) + recovered[:(count-1)*2])
                self.assertEqual(len(decoded), len(pcm))
                # Require low distortion as well as exact compatibility.
                error = audioop.add(pcm, audioop.mul(decoded, 2, -1), 2)
                self.assertLess(audioop.rms(error, 2), max(30, audioop.rms(pcm, 2) * 0.12))
                (output / (path.stem + '.adpcm')).write_bytes(packed)
                (output / (path.stem + '.pcm')).write_bytes(decoded)
                total_original += path.stat().st_size
                total_packed += len(packed)
        self.assertLess(total_packed, total_original / 3.5)
        print(f'Cue packing: {total_original} -> {total_packed} bytes; all nine timings and reference decodes verified')

if __name__ == '__main__':
    unittest.main()
