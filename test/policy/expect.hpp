#pragma once

#include <iostream>
#include <string>

inline int &TestFailures() {
	static int failures = 0;
	return failures;
}

inline void ExpectAt(bool cond, const std::string &msg, const char *file, int line) {
	if (!cond) {
		std::cerr << "FAIL: " << file << ":" << line << ": " << msg << std::endl;
		TestFailures()++;
	}
}

#define Expect(cond, msg) ExpectAt((cond), (msg), __FILE__, __LINE__)
