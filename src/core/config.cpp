#include "keyhunt/core/config.h"

namespace keyhunt::core {

const char *bsgs_modes[5] = {"sequential","backward","both","random","dance"};
const char *modes[7] = {"xpoint","address","bsgs","rmd160","pub2rmd","minikeys","vanity"};
const char *cryptos[3] = {"btc","eth","all"};
const char *publicsearch[3] = {"uncompress","compress","both"};
const char *default_fileName = "addresses.txt";

} // namespace keyhunt::core
