// Prints parse results of G-code lines (stdin, one per line) in a fixed
// format, compared against Klipper's Python parsing by a script.
#include <cstdio>
#include <iostream>
#include <string>

#include "klippy/gcode.hpp"

int
main()
{
    std::string line;
    while (std::getline(std::cin, line)) {
        std::string cmd, cl;
        kl::GCodeCommand::ParamList params;
        kl::GCodeDispatch::parse_line(line, &cmd, &params, &cl);
        printf("[%s]", cmd.c_str());
        for (auto &kv : params)
            printf(" %s=[%s]", kv.first.c_str(), kv.second.c_str());
        printf("\n");
    }
    return 0;
}
