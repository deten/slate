// The single translation unit that instantiates doctest's runner.
//
// Kept in its own static library so the ~320 KB header is compiled once rather
// than in every test translation unit, which matters as the suite grows.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
