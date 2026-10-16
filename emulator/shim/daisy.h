// Stand-in for libDaisy's umbrella header: only the portable pieces the
// firmwares use, plus emulated peripherals (emu_peripherals.h).
#pragma once
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "daisy_core.h"
#include "sys/system.h"
#include "util/FIFO.h"
#include "util/wav_format.h"
#include "ui/UI.h"
#include "hid/midi_parser.h" // MidiEvent.h has no include guard; this has
#include "emu_peripherals.h"
