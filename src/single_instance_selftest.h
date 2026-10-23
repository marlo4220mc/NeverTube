#pragma once

// Offline selftest of the single instance: two objects of the same process with
// the same name, where the second one has to hand its arguments over to the
// first and NOT take the name.
bool singleInstanceSelfTest();
