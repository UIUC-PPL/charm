#include "defines.h"
#include "Particle.h"
#include "util.h"
#include "Node.h"
#include "MultipoleMoments.h"
#include "TreePiece.h"

int log2ceil(int n){
  int cur = 1;
  int ret = 0;
  while(cur < n){
    cur <<= 1;
    ret++;
  }
  return ret;
}

int mssb64_pos(Key x) {
  int n;

  if (x == Key(0)) return -1;
  n = 0;
  if (x > Key(0x00000000FFFFFFFF)) {n += 32; x >>= 32;}
  if (x > Key(0x000000000000FFFF)) {n += 16; x >>= 16;}
  if (x > Key(0x00000000000000FF)) {n += 8; x >>= 8;}
  if (x > Key(0x000000000000000F)) {n += 4; x >>= 4;}
  if (x > Key(0x0000000000000003)) {n += 2; x >>= 2;}
  if (x > Key(0x0000000000000001)) {n += 1;}
  return n;
}

Key mssb64(Key x)
{
  x |= (x >> 1);
  x |= (x >> 2);
  x |= (x >> 4);
  x |= (x >> 8);
  x |= (x >> 16);
  x |= (x >> 32);
  return(x & ~(x >> 1));
}

bool CompareKeys(void *a, Key k){
  Key *ownerKey = (Key *)a;
  return (*ownerKey >= k);
}

bool CompareParticleToKey(void *a, Key k){
  Particle *p = (Particle *)a;
  return (p->key >= k);
}

void findSplitters(Particle *particles, int start, int end, int *splitters, Key childKey, int childDepth){
  int nRankBits = LOG_BRANCH_FACTOR;
  // particles of first child always begin at index 0
  splitters[0] = start;
  // for all other children, set the beginning of each 
  // (and, hence, the end of the previous child)
  for(int i = 1; i < BRANCH_FACTOR; i++){
    childKey++;
    Key testKey = (childKey << (TREE_KEY_BITS-(nRankBits*childDepth+1))); 
    int firstGEIdx = binary_search_ge<Particle>(testKey, particles, start, end, CompareParticleToKey); 
    splitters[i] = firstGEIdx;
    start = firstGEIdx;
  }
  // end of the last child is always at index end 
  // (which is one position past the last particle)
  splitters[BRANCH_FACTOR] = end;
}

ExternalParticle &ExternalParticle::operator=(const Particle &p){
  this->position = p.position;
  this->mass = p.mass;
  return *this;
}

