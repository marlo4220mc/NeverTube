#pragma once

#include <QString>

// Returns the playlist ID if the URL represents a playlist or includes a list= parameter.
QString youtubePlaylistIdFromText(const QString &text);

// Strict reading of a YouTube LINK pasted in a text field (Ctrl+V, Súper+V,
// right-click paste, ...). It returns the 11 characters of the video only when
// the text IS an address of this site: youtube.com/watch?v=, youtu.be/<id>,
// /shorts/<id>, /embed/<id>, /live/<id>, /v/<id>, with or without www./m./
// music., with or without extra parameters (t=, list=, si=) and with or without
// the scheme (the address bar sometimes loses it when copying).
//
// Anything else returns an empty string, and that includes a BARE ID: the
// search box must not turn an 11-letter query into a video. `Yt::videoIdFromUrl`
// is the loose version and it is right for the `http://...` entry point of the
// command line, where the whole argument is already an address.
QString youtubeVideoIdFromText(const QString &text);

// Offline selftest: the accepted forms, the rejected ones and the ids it must
// NOT invent. Pure: no network, no widgets.
bool youtubeUrlSelfTest();
