#pragma once

// Offline self-test of the updater (--selftest-update): the order of two
// versions, the installer that belongs to each build, the reading of
// SHA256SUMS.txt (missing entry, ambiguous entry, near-miss name) and the
// SHA-256 of a file. No network and no GPU: it runs in CI.
bool updateSelfTest();
