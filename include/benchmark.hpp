#pragma once

#include "options.hpp"

void BenchmarkFile(const Options &options, const std::wstring &srcPath, const std::wstring &dstPath);
int Run(const Options &options);