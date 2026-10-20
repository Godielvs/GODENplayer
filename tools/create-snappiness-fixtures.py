"""Private, synthetic artwork/audio fixtures. Never part of a shipped profile."""
import argparse
import json
import subprocess
import wave
from pathlib import Path

import numpy as np
from PIL import Image


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("destination", type=Path)
    parser.add_argument("--ffmpeg", default=r"C:\FFMPEG\bin\ffmpeg.exe")
    args = parser.parse_args()
    args.destination=args.destination.resolve()
    args.destination.mkdir(parents=True, exist_ok=True)
    rate = 48000
    t = np.arange(rate * 18, dtype=np.float64) / rate
    # Quiet phrases, isolated drum-like attacks and a dense middle phrase.
    body = .10 * np.sin(2 * np.pi * 220 * t) * (1 + .45 * np.sin(t * 2))
    pulses = np.exp(-np.remainder(t, .47) * 45)
    body += .24 * pulses * np.sin(2 * np.pi * 65 * t)
    body += ((t > 6) & (t < 12)) * .08 * np.sin(2 * np.pi * 3400 * t)
    samples = np.column_stack([body, body * .85 + .04 * np.sin(t * 2 * np.pi * 331)])
    with wave.open(str(args.destination / "source.wav"), "wb") as stream:
        stream.setparams((2, 2, rate, 0, "NONE", "not compressed"))
        stream.writeframes((samples * 32767).astype("<i2").tobytes())
    manifest = []
    for size, extension, count in [(5000, "jpg", 12), (5000, "png", 2), (2000, "png", 2), (720, "png", 1)]:
        directory = args.destination / f"QA-{size}-{extension}"
        directory.mkdir(exist_ok=True)
        y, x = np.ogrid[:size, :size]
        color = np.empty((size, size, 3), np.uint8)
        rng = np.random.default_rng(7103)
        for channel, offset in enumerate([12, 87, 161]):
            field = (95 + 65 * np.sin(x / 281 + offset) + 44 * np.cos(y / 177 + offset))
            texture = rng.integers(-20, 21, (size, size), dtype=np.int16) if extension=="jpg" else \
                np.repeat(np.repeat(rng.integers(-20,21,((size+4)//5,(size+4)//5),dtype=np.int16),5,axis=0),5,axis=1)[:size,:size]
            color[:, :, channel] = np.clip(field + texture, 0, 255)
        if size == 720:
            color[:] = color[:, :, :1]  # Monochromatic fallback/fidelity case.
        image = Image.fromarray(color)
        art = directory / f"cover.{extension}"
        image.save(art, quality=94)
        del color, image
        for track in range(1, count + 1):
            audio = directory / f"{track:02d}-fixture.flac"
            subprocess.run([args.ffmpeg, "-hide_banner", "-loglevel", "error", "-y",
                            "-i", str(args.destination / "source.wav"), "-i", str(art),
                            "-map", "0:a", "-map", "1:v", "-c:a", "flac", "-c:v", "copy",
                            "-disposition:v", "attached_pic", "-metadata", "artist=Goden QA",
                            "-metadata", f"album={directory.name}", "-metadata", f"title=Fixture {track}",
                            "-metadata", f"track={track}", str(audio)], check=True)
        manifest.append(dict(directory=str(directory), edge=size, format=extension,
                             tracks=count, encodedBytes=art.stat().st_size))
    (args.destination / "fixtures.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    print(json.dumps(manifest))


if __name__ == "__main__":
    main()
