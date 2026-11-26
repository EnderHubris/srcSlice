// SPDX-License-Identifier: GPL-3.0-only
/**
 * @file main.cpp
 *
 * @copyright Copyright (C) 2018-2024 srcML, LLC. (www.srcML.org)
 *
 * This file is part of the srcSlice application.
 */

#include <srcslicehandler.hpp>

// determine default max threads
int defaultThreads() {
    int processorCount = CPUCount();
    if (processorCount < 2)
        return 2;
    else if (processorCount == 2)
        return 3;
    else
        return processorCount + 2;
}

int main(int argc, char **argv)
{
    // search for the output flag and set a marker
    std::string inputFile = "", outputFile = "";
    int threadCount = defaultThreads();
    int iterations = 1;
    std::ofstream outFile;
    bool verboseMode = false, feedbackMode = false,
        showControlEdges = false, expandAliases = false;

    std::ostringstream ss;
    ss << "Number of concurrent threads [Default " << threadCount << "]";

    CLI::App app{"srcSlice (srcML Slicing Tool)"};
    // Options hold extra data
    app.add_option  ("input",   inputFile,                          "Name of the srcML input file [Must be built using the --position and --hash flags]")
        ->required()
        ->type_name("srcML FILE");
    app.add_option  ("-o, --output",  outputFile,                   "Name of the JSON output file [Stdout is Default]");
    app.add_option  ("-t, --threads", threadCount,                  ss.str())
        ->default_val(threadCount);
    app.add_option  ("-i, --iterations",  iterations,               "Number of iterations of interprocedural")
        ->default_val(iterations);

    app.add_flag    ("-c, --control-edges", showControlEdges,       "Display Control-Edges of the Slice");
    app.add_flag    ("-a, --expand-aliases", expandAliases,         "Expand Alias list");
    app.add_flag    ("-v, --verbose", verboseMode,                  "Display Debug Info when Slicing");
    app.add_flag    ("-p, --progress", feedbackMode,                "Display Feedback Progress Bars");
    
    CLI11_PARSE(app, argc, argv);

    CliInfo info = {
        inputFile, outputFile,
        threadCount,
        showControlEdges,
        iterations,
        expandAliases,
        verboseMode,
        feedbackMode
    };

    try {
        // test if options are of valid value(s)
        if (threadCount < 1) {
            throw std::runtime_error("Thread count must be a non-zero positive integer");
        } else if (iterations < 1) {
            throw std::runtime_error("Iterations must be a non-zero positive integer");
        }
        
        // check if input file exists
        std::ifstream srcmlFile(inputFile);
        if (!srcmlFile.is_open()) {
            throw std::runtime_error("Input file does not exist");
        }
        srcmlFile.close();

        if (showControlEdges) {
            std::cout << "[\033[33mWARNING\033[0m] Computing control-edges may increase the time required to complete the program" << std::endl;
        }

        if (expandAliases) {
            std::cout << "[\033[33mWARNING\033[0m] Expanding Pointer Aliasing may increase the time required to complete the program" << std::endl;
        }

        SrcSliceHandler srcSliceHandler(info);

        // write to either stdout or output file
        if (!outputFile.empty()) {
            // allow output file to be written in
            outFile = std::ofstream(outputFile);
            std::cout << "Writing to :: " << outputFile << "\n";
            srcSliceHandler.ToJSON(outFile);
            std::cout << "Output Saved to :: " << outputFile << "\n";
            outFile.close();
        } else {
            // write to stdout
            srcSliceHandler.ToJSON(std::cout);
        }
    } catch (const std::exception& e) {
        std::cout << "\033[31m" << e.what() << "\033[0m" << std::endl;
        return 3;
    }
}