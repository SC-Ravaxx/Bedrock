#pragma once
// What the program carries inside itself, by name - the glyph sets, the
// second reader's model and alphabet, the self-test's pictures - on both
// platforms.
//
// Windows keeps them as RCDATA resources (resources/bedrock.rc) and this
// asks FindResource. Linux has no resource section; there the same files
// are assembled into .rodata with .incbin (see EmbeddedData.cpp), so a
// Linux binary is as self-contained as the exe and a fresh install reads
// with the same templates and the same model.
#include <cstddef>
#include <string>

namespace SCMiner {

// The bytes behind a name, or false. The bytes belong to the program and
// live as long as it does; nobody frees them.
bool embeddedResource(const std::string& name, const unsigned char** data, size_t* size);

}  // namespace SCMiner
