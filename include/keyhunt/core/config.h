#pragma once

#include <string>

namespace keyhunt::core {

// Numeric values preserve the established CLI and CPU dispatch tables.
inline constexpr int CRYPTO_NONE = 0;
inline constexpr int CRYPTO_BTC = 1;
inline constexpr int CRYPTO_ETH = 2;
inline constexpr int CRYPTO_ALL = 3;

inline constexpr int MODE_XPOINT = 0;
inline constexpr int MODE_ADDRESS = 1;
inline constexpr int MODE_BSGS = 2;
inline constexpr int MODE_RMD160 = 3;
inline constexpr int MODE_PUB2RMD = 4;
inline constexpr int MODE_MINIKEYS = 5;
inline constexpr int MODE_VANITY = 6;

inline constexpr int SEARCH_UNCOMPRESS = 0;
inline constexpr int SEARCH_COMPRESS = 1;
inline constexpr int SEARCH_BOTH = 2;

// Input preferences only. Progress counters, caches and arithmetic working state
// belong to the executor. The CLI resolves mode-dependent defaults before work.
// Range normalization is still in the CPU adapter until C05 supplies its contract.
struct SearchConfig {
    int skip_checksum = 0;
    int endomorphism = 0;
    int bloom_multiplier = 1;
    int vanity = 0;
    int base_minikey = 0;
    int bsgs_order = 0;
    int debug = 0;
    int quiet = 0;
    int matrix = 0;
    int bsgs_k_factor = 1;
    int threads = 1;
    int cache_targets = 0;
    int has_stride = 0;
    int encoding = 2;
    int has_bit_range = 0;
    int has_range = 0;
    int has_target_file = 0;
    int mode = MODE_ADDRESS;
    int crypto = 0;
    int random = 0;
    int has_batch_size = 0;
    int bit_range = 0;
    std::string target_file = "addresses.txt";
    std::string batch_size;
    std::string stride;
};

extern const char *bsgs_modes[5];
extern const char *modes[7];
extern const char *cryptos[3];
extern const char *publicsearch[3];
extern const char *default_fileName;

} // namespace keyhunt::core
