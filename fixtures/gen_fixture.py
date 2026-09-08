#!/usr/bin/env python3
"""Generate the self-authored playback fixture retrovert_selftest.kss.

A KSSX tune whose MSX player is Z80 written out by hand below: init opens
the three PSG tone channels, and the play routine, called once per frame,
walks a four-chord figure. Deterministic output — the committed fixture
and its sha256 in harness.toml must match what this script emits.
"""

import struct
from pathlib import Path

OUT = Path(__file__).parent / "retrovert_selftest.kss"

# libkss maps the payload at LOAD and leaves 0x8000..0xbfff read-only when
# ram_mode is off, so the code sits in bank 1 and its one variable in bank 3.
LOAD = 0x4000
INIT = 0x4000
PLAY = 0x4040
TABLE = 0x4080
LOAD_LEN = 0x00C0
FRAME = 0xC000  # one byte of scratch RAM

WRTPSG = 0x0093  # libkss plants the MSX BIOS entry here: A = register, E = value

PSG_MIXER, PSG_VOL_A = 7, 8
MIXER_TONES_ONLY = 0xB8  # three tone channels on, noise off
VOLUME = 15  # of 15

# PSG period = 1789772.5 / (16 * frequency).
CLOCK = 1789772.5
FIGURE = [
    [261.63, 329.63, 392.00],  # C4 E4 G4
    [293.66, 349.23, 440.00],  # D4 F4 A4
    [329.63, 392.00, 493.88],  # E4 G4 B4
    [261.63, 329.63, 523.25],  # C4 E4 C5
]
STRIDE = 8  # bytes per chord in the table, a power of two so indexing is a shift


def ld_a(v):
    return [0x3E, v]


def ld_e(v):
    return [0x1E, v]


def call(addr):
    return [0xCD, addr & 0xFF, addr >> 8]


def write_psg(register, value):
    return ld_a(register) + ld_e(value) + call(WRTPSG)


def init_routine():
    code = write_psg(PSG_MIXER, MIXER_TONES_ONLY)
    for channel in range(3):
        code += write_psg(PSG_VOL_A + channel, VOLUME)
    code += [0xAF]  # XOR A
    code += [0x32, FRAME & 0xFF, FRAME >> 8]  # LD (FRAME),A
    code += [0xC9]  # RET
    return code


def play_routine():
    # Bits 5-6 of the frame counter pick the chord, so the figure advances
    # every 32 frames -- about 0.53 s at the MSX 60 Hz vsync.
    code = [0x3A, FRAME & 0xFF, FRAME >> 8]  # LD A,(FRAME)
    code += [0x3C]  # INC A
    code += [0x32, FRAME & 0xFF, FRAME >> 8]  # LD (FRAME),A
    code += [0xCB, 0x3F] * 5  # SRL A x5
    code += [0xE6, 0x03]  # AND 3
    code += [0x87] * 3  # ADD A,A x3 -> times STRIDE
    code += [0x21, TABLE & 0xFF, TABLE >> 8]  # LD HL,TABLE
    code += [0x85, 0x6F]  # ADD A,L ; LD L,A
    for register in (0, 2, 4):  # tone period lo/hi for channels A, B, C
        for half in (0, 1):
            code += [0x7E, 0x5F]  # LD A,(HL) ; LD E,A
            code += ld_a(register + half) + call(WRTPSG)
            code += [0x23]  # INC HL
    code += [0xC9]  # RET
    return code


def note_table():
    out = []
    for chord in FIGURE:
        entry = []
        for hz in chord:
            period = round(CLOCK / (16 * hz))
            entry += [period & 0xFF, period >> 8]
        out += entry + [0] * (STRIDE - len(entry))
    return out


def z80_image():
    image = bytearray(LOAD_LEN)
    for addr, code in ((INIT, init_routine()), (PLAY, play_routine()), (TABLE, note_table())):
        end = addr - LOAD + len(code)
        assert end <= LOAD_LEN, f"block at {addr:04x} overruns the image"
        image[addr - LOAD:end] = bytes(code)
    return bytes(image)


def build():
    header = bytearray(0x10)
    header[0:4] = b"KSSX"
    struct.pack_into("<HHHH", header, 4, LOAD, LOAD_LEN, INIT, PLAY)
    header[0x0C] = 0  # first extra bank
    header[0x0D] = 0  # no extra banks
    header[0x0E] = 0  # no extended header, so the payload starts at 0x10
    header[0x0F] = 0  # MSX, PSG only
    return bytes(header) + z80_image()


def main():
    data = build()
    OUT.write_bytes(data)
    print(f"wrote {OUT} ({len(data)} bytes)")


if __name__ == "__main__":
    main()
