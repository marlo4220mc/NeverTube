#pragma once

// Integration test of the download BUTTON: it builds the real MainWindow,
// finds the button of the Player by its name, presses it, and checks that
// the download was queued (in the repository, with the state "pending").
//
// It touches NO network: the queue is what is checked, not the download.
// The download itself is checked by the manual test (a real video, a real
// file) and by the offline plan selftest.
bool downloadButtonSelfTest();
