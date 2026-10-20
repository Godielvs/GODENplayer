# SQLite 3.53.3

The files `sqlite3.c`, `sqlite3.h`, and `sqlite3ext.h` are the official SQLite
amalgamation `3530300`, downloaded from:

`https://www.sqlite.org/2026/sqlite-amalgamation-3530300.zip`

The archive SHA3-256 is:

`d45c688a8cb23f68611a894a756a12d7eb6ab6e9e2468ca70adbeab3808b5ab9`

SQLite is in the public domain. See `https://www.sqlite.org/copyright.html`.

NekoRoX builds it as a private static library. The database owner uses one
connection on one background thread; playback and audio callbacks never call
SQLite.
