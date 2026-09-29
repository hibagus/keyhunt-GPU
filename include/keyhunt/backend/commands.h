#pragma once

namespace keyhunt::backend {
// Returns -1 for the preserved legacy CLI; recognized subcommands own their
// parsing and never fall through into a CPU search after a GPU error.
int dispatch_command(int argc, char** argv);
}
