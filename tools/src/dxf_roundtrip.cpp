/// Read a DXF and write it straight back out.
///
/// A headless check on the read/write path, useful for eyeballing a real
/// drawing's output and for scripted comparison against an independent reader.
///
///     dxf_roundtrip in.dxf out.dxf

#include <fstream>
#include <iostream>
#include <sstream>

#include "cad/io/DxfReader.hpp"
#include "cad/io/DxfWriter.hpp"

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: dxf_roundtrip <input.dxf> <output.dxf>\n";
        return 2;
    }

    std::ifstream input{argv[1], std::ios::binary};
    if (!input) {
        std::cerr << "cannot read " << argv[1] << '\n';
        return 1;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();

    try {
        cad::io::DxfReadResult result = cad::io::readDxf(buffer.str());

        std::size_t warnings = 0;
        for (const cad::io::DxfDiagnostic& diagnostic : result.diagnostics) {
            if (diagnostic.severity != cad::io::DxfDiagnostic::Severity::Info) {
                ++warnings;
            }
        }

        std::cout << "read  " << argv[1] << ": " << result.entityCount << " entities, "
                  << result.database->layers().size() << " layers, " << warnings
                  << " warning(s)\n";

        cad::io::DxfWriter writer;
        const std::string text = writer.write(*result.database);

        std::ofstream output{argv[2], std::ios::binary};
        if (!output) {
            std::cerr << "cannot write " << argv[2] << '\n';
            return 1;
        }
        output.write(text.data(), static_cast<std::streamsize>(text.size()));

        for (const cad::io::DxfDiagnostic& diagnostic : writer.diagnostics()) {
            std::cerr << "  write warning: " << diagnostic.message << '\n';
        }
        std::cout << "wrote " << argv[2] << ": " << text.size() << " bytes\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "failed: " << error.what() << '\n';
        return 1;
    }
}
