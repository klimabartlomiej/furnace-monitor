// The public halves of the release signing keys (release/keygen.ps1): RemoteUpdate.h installs a release only when
// its manifest is signed with one of them. The first signs every release; the second is the spare, kept off the
// release PC. Changing these locks out the ESPs that have the old ones - they take the new keys only over USB.

#pragma once

static const char* const RELEASE_KEYS[] = {
  "0437c8c53c96f92d4d282f56386827f3afc078466d1acddd75b6d787bc63805488f77e40c02c39e4e8e59c43d34ac8be9a454b7910dd55ab1d64ed12dcf053c7b6",
  "046e1ff90eda81e29f1c4b9855185ff4bda39e2071c8266060f5808c77d278bf372ae52eac631ad6344f2f20b1cdb4dfacc7d16151ca6197a447e07f66a164e433",
};

// A manifest in publish.ps1's format, signed with the first key: the ESP checks its verifier on it (and on a
// forged copy) before it demands signatures.
static const char RELEASE_SELFTEST[] = R"({"version":1,"size":1,"sha256":"0000000000000000000000000000000000000000000000000000000000000000","url":"-","mirror":"-","min":0,"sig":"cbd261df62eb5638b17d4702ab2fa23778d678da40fc4e9dde790323f6cb401aab4f0f499c2a3f4bb3a4421edd0242bbc23003e246ef385bd0699f20187be1ba","notes":{}})";
