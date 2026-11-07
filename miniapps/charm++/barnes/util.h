#ifndef __UTIL_H__
#define __UTIL_H__

struct ForceData;
template<typename T>
class Node;

int log2ceil(int n);
Key mssb64(Key x);
int mssb64_pos(Key x);

void findSplitters(Particle *particles, int start, int end, int *splitters, Key childKey, int childDepth);

typedef bool (*BinarySearchGEFn)(void *, Key k);

template<typename T>
int binary_search_ge(Key check, T *particles, int start, int end, BinarySearchGEFn fn){
  int lo = start;
  int hi = end;
  int mid;
  while(lo < hi){
    mid = lo+((hi-lo)>>1);
    if((*fn)(particles+mid,check)){
      hi = mid;
    }
    else{
      lo = mid+1;
    }
  }
  return lo;
}

void getMomentsFromParticles(Node<ForceData> *bucket);
void getMomentsFromChildren(Node<ForceData> *node);
void getBoundingBoxFromParticles(Node<ForceData> *node);
void getBoundingBoxFromChildren(Node<ForceData> *node);
#endif
