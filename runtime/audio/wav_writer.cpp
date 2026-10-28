#include "wav_writer.h"

namespace x2 {

namespace {
void put16(std::FILE* f, uint32_t v) {
    std::fputc(int(v & 0xFF), f);
    std::fputc(int((v >> 8) & 0xFF), f);
}
void put32(std::FILE* f, uint32_t v) {
    put16(f, v & 0xFFFF);
    put16(f, v >> 16);
}
} // namespace

bool WavWriter::open(const std::string& path, int rate, int channels) {
    close();
    f_ = std::fopen(path.c_str(), "wb");
    if (!f_) return false;
    data_bytes_ = 0;
    std::fwrite("RIFF", 1, 4, f_);
    put32(f_, 36);
    std::fwrite("WAVEfmt ", 1, 8, f_);
    put32(f_, 16);
    put16(f_, 1); // PCM
    put16(f_, uint32_t(channels));
    put32(f_, uint32_t(rate));
    put32(f_, uint32_t(rate * channels * 2));
    put16(f_, uint32_t(channels * 2));
    put16(f_, 16);
    std::fwrite("data", 1, 4, f_);
    put32(f_, 0);
    return true;
}

void WavWriter::write(const int16_t* samples, size_t count) {
    if (!f_) return;
    for (size_t i = 0; i < count; ++i) put16(f_, uint16_t(samples[i]));
    data_bytes_ += uint32_t(count * 2);
}

void WavWriter::close() {
    if (!f_) return;
    std::fseek(f_, 4, SEEK_SET);
    put32(f_, 36 + data_bytes_);
    std::fseek(f_, 40, SEEK_SET);
    put32(f_, data_bytes_);
    std::fclose(f_);
    f_ = nullptr;
}

} // namespace x2
