#pragma once

#include "movie/avc_decoder.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

// A movie a mod puts in place of one of the game's: a raw H.264 stream
// (Annex B, ".264"), usually larger and sharper, shown picture for picture
// instead of the game's own while the game plays its own sound. The file is
// read a piece at a time, so a long movie at a high resolution costs little
// memory.
namespace portablekit::movie {

class ReplacementMovie {
public:
    bool open(const std::filesystem::path &path);
    [[nodiscard]] bool is_open() const noexcept { return file_.is_open(); }

    // Picture number `index` (0 first), decoding forward to it; a request for
    // a picture already passed gets the latest one. False once the movie has
    // no more pictures, or on an error.
    bool picture(std::uint64_t index, Picture &out);

private:
    bool refill();
    bool next_nal(std::vector<std::uint8_t> &nal);
    bool next_unit(std::vector<std::uint8_t> &unit);
    bool next_picture();

    std::ifstream file_;
    std::vector<std::uint8_t> data_;
    std::size_t at_{};
    bool end_of_file_{};
    std::vector<std::uint8_t> pending_;  // a NAL unit that starts the next access unit
    AvcDecoder decoder_;
    Picture current_;
    std::uint64_t produced_{};  // pictures decoded so far
    bool finished_{};
};

} // namespace portablekit::movie
