#include "Node.h"
#include "State.h"
#include "TreePiece.h"

void State::nodeEncountered(Key bucketKey, Node<ForceData> *node){
}

void State::nodeOpened(Key bucketKey, Node<ForceData> *node){
}

void State::nodeDiscarded(Key bucketKey, Node<ForceData> *node){
  insert(bucketKey,node->getKey());
}

void State::nodeComputed(Node<ForceData> *bucket, Key nodeKey){
 insert(bucket->getKey(),nodeKey);
}

void State::bucketComputed(Node<ForceData> *bucket, Key k){
 insert(bucket->getKey(),k);
}


