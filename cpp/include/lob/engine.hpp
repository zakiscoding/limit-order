#pragma once

#include "lob/book.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace lob {

std::vector<Event> generate(uint64_t seed, uint64_t count);

void write_binary(const std::string& path, uint64_t seed, const std::vector<Event>& events);
std::vector<Event> read_binary(const std::string& path, uint64_t* seed);
std::vector<Event> read_text(const std::string& path);
std::vector<Event> read_any(const std::string& path, uint64_t* seed);

std::string fingerprint_line(uint64_t index, const Book& book);

struct TypeCounts {
    uint64_t add = 0;
    uint64_t cancel = 0;
    uint64_t modify = 0;
    uint64_t reset = 0;
    uint64_t other = 0;
};

TypeCounts count_types(const std::vector<Event>& events);

void replay_to_file(const std::string& input, const std::string& output, uint64_t checkpoint, bool verify);

int check_spec(const std::string& scenarios_path, const std::string& expected_path);

}  // namespace lob
