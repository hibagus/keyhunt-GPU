// Test-only line protocol for the independent Python integer oracle.
#include "keyhunt/core/exact_range.h"

#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace keyhunt::core;

namespace {
std::string evaluate(const std::vector<std::string>& words) {
    if (words.size() != 3) throw std::invalid_argument("expected operation and two values");
    const auto a = UInt256::from_hex(words[1]);
    const auto b = UInt256::from_hex(words[2]);
    const auto& operation = words[0];
    if (operation == "add") return a.add(b).hex();
    if (operation == "sub") return a.subtract(b).hex();
    if (operation == "mul") return a.multiply(b).hex();
    if (operation == "div") {
        const auto result = a.divmod(b);
        return result.first.hex() + " " + result.second.hex();
    }
    if (operation == "range") {
        const ScalarInterval range(a, b);
        return range.size().hex();
    }
    throw std::invalid_argument("unknown operation");
}
}

int main() {
    for (std::string line; std::getline(std::cin, line);) {
        std::istringstream input(line);
        std::vector<std::string> words;
        for (std::string word; input >> word;) words.push_back(word);
        try {
            const auto result = evaluate(words); // Avoid a partial response on failure.
            std::cout << "ok " << result << '\n';
        } catch (const std::exception&) {
            std::cout << "error\n";
        }
    }
}
