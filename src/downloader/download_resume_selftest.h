#pragma once

// The selftest of the RESUME of a download and of the migration of the rows of
// the first version of the downloader (the mp3 shared the row of the video).
//
// It is offline: the walk that decides which fragment is asked for is a pure
// function, and the migration only touches a temporary database and a
// temporary folder. It runs in the smoke of the three CI and in the recipes of
// packaging/linux.
bool downloadResumeSelfTest();
