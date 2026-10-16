// Headless driver for the emulator: boots a machine on a card folder, runs a
// script of panel/MIDI actions, and writes the line output to a 48 kHz WAV.
//
//   chompi_render [--firmware tape|wave|tempo] <card-folder> <out.wav> "<script>"
//
// Script commands, separated by ';':
//   wait <ms>            run the machine
//   press <id> | release <id> | tap <id>      switch IDs from chompi/Panel.h
//   turn <encoder> <detents>
//   toggle <1 play | 0 record>
//   note <num> [vel] | off <num>              MIDI channel 1
//   cc <num> <val>
//   midi <hex bytes>     any message, e.g. "midi fa" (start)
//   clock <bpm>          MIDI clock (24 PPQN) during waits; 0 stops it
//   sync <1 | 0>         the firmware's clock sync mode (TEMPO)
//   input <off | sine <hz> <gain>>            line input signal
//   leds                 print LED state
//   recordings [dir]     print the RAM recordings (TAPE); with dir, write <dir>/<name>.raw
//   restore [<name> <file.raw>]...   restore recordings (int16 stereo); unnamed ones reset
// CHOMPI_HP=1 renders the headphone output instead of the line output.
// Note: the firmware writes options.json/presets.json at boot; use a copy.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "chompi/Machine.h"
#include "chompi/VirtualCard.h"

namespace
{
struct Runner
{
    emu::Machine&      machine;
    std::vector<float> out_l, out_r;
    double             input_hz = 0, input_gain = 0, phase = 0;
    double             clock_bpm = 0, clock_phase = 0; // in pulses
    float              peak     = 0;
    bool               headphones = []() {
        const char* hp = std::getenv("CHOMPI_HP");
        return hp != nullptr && *hp != '\0' && *hp != '0';
    }();

    void Run(double ms)
    {
        const int blocks = int(ms * emu::Machine::kSampleRate / 1000.0 / emu::Machine::kBlockSize);
        float     in[4][emu::Machine::kBlockSize]  = {};
        float     out[4][emu::Machine::kBlockSize] = {};
        const float* ins[4]  = {in[0], in[1], in[2], in[3]};
        float*       outs[4] = {out[0], out[1], out[2], out[3]};

        for(int b = 0; b < blocks; b++)
        {
            // Clock pulses due in this block go in before it, as the plugin's
            // block-accurate MIDI timing would deliver them.
            if(clock_bpm > 0)
            {
                clock_phase += clock_bpm * 24.0 / 60.0 * emu::Machine::kBlockSize / emu::Machine::kSampleRate;
                const uint8_t pulse = 0xf8;
                for(; clock_phase >= 1.0; clock_phase -= 1.0)
                    machine.SendMidi(&pulse, 1);
            }
            for(int i = 0; i < emu::Machine::kBlockSize; i++)
            {
                float s = 0;
                if(input_hz > 0)
                {
                    s = float(input_gain * std::sin(phase));
                    phase = std::fmod(phase + 2 * M_PI * input_hz / emu::Machine::kSampleRate, 2 * M_PI);
                }
                in[0][i] = in[2][i] = in[3][i] = s;
            }
            machine.Process(ins, outs);
            for(int i = 0; i < emu::Machine::kBlockSize; i++)
            {
                const int ch = headphones ? 0 : 2;
                out_l.push_back(out[ch][i]);
                out_r.push_back(out[ch + 1][i]);
                peak = std::max(peak, std::max(std::fabs(out_l.back()), std::fabs(out_r.back())));
            }
            uint8_t msg[3];
            while(const int size = machine.ReceiveMidi(msg))
            {
                std::printf("  midi out @%.4f", out_l.size() / double(emu::Machine::kSampleRate));
                for(int i = 0; i < size; i++)
                    std::printf(" %02x", msg[i]);
                std::printf("\n");
            }
        }
    }

    void PrintLeds()
    {
        std::printf("  pth:");
        for(int i = 0; i < emu::panel::kNumPthLeds; i++)
            std::printf(" %06x", machine.PthLed(i));
        std::printf("\n  smt:");
        for(int i = 0; i < emu::panel::kNumSmtLeds; i++)
            std::printf(" %06x", machine.SmtLed(i));
        std::printf("\n");
    }
};

void WriteWav(const std::string& path, const std::vector<float>& l, const std::vector<float>& r)
{
    std::ofstream f(path, std::ios::binary);
    auto          u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto          u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    const uint32_t data_bytes = uint32_t(l.size() * 2 * 2);
    f.write("RIFF", 4);
    u32(36 + data_bytes);
    f.write("WAVEfmt ", 8);
    u32(16);
    u16(1);
    u16(2);
    u32(48000);
    u32(48000 * 4);
    u16(4);
    u16(16);
    f.write("data", 4);
    u32(data_bytes);
    for(size_t i = 0; i < l.size(); i++)
    {
        u16(uint16_t(int16_t(std::lround(std::fmax(-1.f, std::fmin(1.f, l[i])) * 32767))));
        u16(uint16_t(int16_t(std::lround(std::fmax(-1.f, std::fmin(1.f, r[i])) * 32767))));
    }
}
} // namespace

int main(int argc, char** argv)
{
    auto firmware = emu::Firmware::Tape;
    if(argc > 2 && std::string(argv[1]) == "--firmware")
    {
        const std::string name = argv[2];
        firmware = name == "wave" ? emu::Firmware::Wave : name == "tempo" ? emu::Firmware::Tempo : emu::Firmware::Tape;
        argc -= 2;
        argv += 2;
    }
    if(argc < 4)
    {
        std::fprintf(stderr, "usage: %s [--firmware tape|wave|tempo] <card-folder> <out.wav> \"<script>\"\n", argv[0]);
        return 2;
    }

    auto machine_ptr = emu::Machine::Create(firmware, emu::VirtualCard::Open(argv[1]));
    if(machine_ptr == nullptr)
    {
        std::fprintf(stderr, "that firmware isn't built in\n");
        return 2;
    }
    emu::Machine& machine = *machine_ptr;
    Runner        runner{machine};

    std::stringstream script(argv[3]);
    std::string       command;
    while(std::getline(script, command, ';'))
    {
        std::istringstream in(command);
        std::string        op;
        if(!(in >> op))
            continue;
        std::printf("[%7.1f s] %s\n", runner.out_l.size() / 48000.0, command.c_str());

        auto button = [&](int id, bool down) {
            machine.Post({emu::PanelEvent::Type::Button, int16_t(id), int16_t(down)});
        };

        if(op == "wait")
        {
            double ms;
            in >> ms;
            runner.Run(ms);
        }
        else if(op == "press" || op == "release" || op == "tap")
        {
            int id;
            in >> id;
            if(op != "release")
                button(id, true);
            if(op == "tap")
            {
                runner.Run(60);
                button(id, false);
            }
            if(op == "release")
                button(id, false);
        }
        else if(op == "turn")
        {
            int enc, detents;
            in >> enc >> detents;
            machine.Post({emu::PanelEvent::Type::Turn, int16_t(enc), int16_t(detents)});
        }
        else if(op == "toggle")
        {
            int play;
            in >> play;
            machine.Post({emu::PanelEvent::Type::Toggle, 0, int16_t(play)});
        }
        else if(op == "note")
        {
            int num, vel = 100;
            in >> num >> vel;
            const uint8_t msg[3] = {0x90, uint8_t(num), uint8_t(vel)};
            machine.SendMidi(msg, 3);
        }
        else if(op == "off")
        {
            int num;
            in >> num;
            const uint8_t msg[3] = {0x80, uint8_t(num), 0};
            machine.SendMidi(msg, 3);
        }
        else if(op == "cc")
        {
            int num, val;
            in >> num >> val;
            const uint8_t msg[3] = {0xb0, uint8_t(num), uint8_t(val)};
            machine.SendMidi(msg, 3);
        }
        else if(op == "midi")
        {
            uint8_t      msg[3];
            int          size = 0;
            unsigned int byte;
            while(size < 3 && in >> std::hex >> byte)
                msg[size++] = uint8_t(byte);
            machine.SendMidi(msg, size);
        }
        else if(op == "input")
        {
            std::string kind;
            in >> kind;
            runner.input_hz = 0;
            if(kind == "sine")
                in >> runner.input_hz >> runner.input_gain;
        }
        else if(op == "clock")
            in >> runner.clock_bpm;
        else if(op == "sync")
        {
            int on;
            in >> on;
            machine.SetClockSync(on != 0);
        }
        else if(op == "leds")
            runner.PrintLeds();
        else if(op == "recordings")
        {
            std::string dir;
            in >> dir;
            emu::Recordings recordings;
            machine.CaptureRecordings(recordings);
            for(const auto& [name, samples] : recordings)
            {
                const auto hash = std::hash<std::string_view>{}(
                    std::string_view(reinterpret_cast<const char*>(samples.data()), samples.size() * 2));
                std::printf("  %s: %zu frames, hash %016zx\n", name.c_str(), samples.size() / 2, hash);
                if(!dir.empty())
                    std::ofstream(dir + "/" + name + ".raw", std::ios::binary)
                        .write(reinterpret_cast<const char*>(samples.data()), std::streamsize(samples.size() * 2));
            }
        }
        else if(op == "restore")
        {
            emu::Recordings recordings;
            std::string     name, file;
            while(in >> name >> file)
            {
                std::ifstream raw(file, std::ios::binary | std::ios::ate);
                auto&         samples = recordings[name];
                samples.resize(size_t(raw.tellg()) / 2);
                raw.seekg(0);
                raw.read(reinterpret_cast<char*>(samples.data()), std::streamsize(samples.size() * 2));
            }
            machine.RestoreRecordings(std::move(recordings));
        }
        else
            std::fprintf(stderr, "unknown command: %s\n", op.c_str());
    }

    WriteWav(argv[2], runner.out_l, runner.out_r);
    std::printf("booted=%d peak=%.4f samples=%zu\n", machine.IsBooted(), runner.peak, runner.out_l.size());
    return 0;
}
