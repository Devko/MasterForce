// The suite: every stage against what it promises, then the chain and LEARN.
#include "check.h"

#include <cstdio>

int eft::g_fail = 0, eft::g_pass = 0;

void limiterTests();
void loudnessTests();
void stageTests();
void learnTests();
void masterTests();

int main() {
    limiterTests();
    loudnessTests();
    stageTests();
    learnTests();
    masterTests();
    std::printf("%d passed, %d failed\n", eft::g_pass, eft::g_fail);
    return eft::g_fail ? 1 : 0;
}
