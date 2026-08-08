#pragma once

// Run keeper_updater in --check-only mode and WAIT for it. Call this BEFORE anything reads data_free
// (translations, tileset, content) so repaired files are picked up by the loads that follow - repairing
// afterwards would not take effect until the next launch.
//
// Returns the updater's exit code:
//    0  verified, nothing to do          10  files were repaired
//   20  check skipped (not configured, offline, no manifest)
//   30  something could not be repaired
//   -1  the updater is not there, or could not be started
//
// Never fatal: every failure path returns and the game continues.
int rarRunIntegrityCheck();
