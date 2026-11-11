#include <fcntl.h>
#include <io.h>
#include <stdio.h>

// Exported checkpoints are bytes; text mode would translate their newlines.
void binary_streams() {
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
}
