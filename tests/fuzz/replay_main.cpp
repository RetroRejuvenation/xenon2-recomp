// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// Stand-in for libFuzzer's main(): runs LLVMFuzzerTestOneInput over every file named on the command line (a directory stands for its
// files, in name order) so the same targets run as plain ctest tests over a seed corpus on every compiler and platform. Options
// (arguments starting with '-', as libFuzzer's) are ignored.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

namespace fs = std::filesystem;

int main(int argc, char** argv) {
    std::vector<std::string> files;
    for (int i = 1; i < argc; ++i) {
        if (argv[i][0] == '-') continue;
        std::error_code ec;
        if (fs::is_directory(argv[i], ec)) {
            std::vector<std::string> in_dir;
            for (const fs::directory_entry& e : fs::directory_iterator(argv[i], ec))
                if (e.is_regular_file()) in_dir.push_back(e.path().string());
            std::sort(in_dir.begin(), in_dir.end());
            files.insert(files.end(), in_dir.begin(), in_dir.end());
        } else {
            files.push_back(argv[i]);
        }
    }
    if (files.empty()) {
        std::fprintf(stderr, "usage: %s CORPUS_DIR_OR_FILE...\n", argv[0]);
        return 2;
    }
    size_t bytes = 0;
    for (const std::string& f : files) {
        std::ifstream in(f, std::ios::binary);
        const std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        static const uint8_t kNone = 0;
        LLVMFuzzerTestOneInput(data.empty() ? &kNone : data.data(), data.size());
        bytes += data.size();
    }
    // The empty input and a few degenerate ones, as libFuzzer tries first.
    static const uint8_t kZeros[64] = {};
    LLVMFuzzerTestOneInput(kZeros, 0);
    LLVMFuzzerTestOneInput(kZeros, 1);
    LLVMFuzzerTestOneInput(kZeros, sizeof kZeros);
    std::printf("%zu inputs, %zu bytes: no crash\n", files.size(), bytes);
    return 0;
}
