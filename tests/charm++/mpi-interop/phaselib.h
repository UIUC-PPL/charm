#ifndef PHASELIB_H
#define PHASELIB_H
// Runs one Charm++ phase: an array broadcast and a sum reduction. Called by
// every rank; returns when the phase has ended (CkExit -> LibCkExit).
void PhaseRun(int iter);
// The reduction result of the last phase; meaningful on PE 0 only.
long PhaseResult();
// The result PhaseRun(iter) must produce.
long PhaseExpected(int iter);
#endif
