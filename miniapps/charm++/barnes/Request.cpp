#include "Request.h"

#include "State.h"
#include "Worker.h"
#include "Traversal_decls.h"
#include "Node.h"

#include "TreePiece.h"

void Request::deliverParticles(int num){
  for(int i = 0; i < requestors.length(); i++){
    Requestor &req = requestors[i];
    CutoffWorker<ForceData> *worker = req.worker;

    void *saveContext = worker->getContext();
    worker->setContext(req.context);

    ExternalParticle *externalParticles = (ExternalParticle *)data;
    for(int j = 0; j < num; j++){ 
      CkAssert(!isnan((externalParticles+j)->position.length()));
      worker->work(externalParticles+j);
    }
    worker->bucketDone(parent->getKey());

    worker->setContext(saveContext);
    State *state = req.state;
    if(state->decrPending()){
      worker->done();
    }
  }

  requestors.clear();
}

void Request::deliverNode(){
  for(int i = 0; i < requestors.length(); i++){
    Requestor &req = requestors[i];
    CutoffWorker<ForceData> *worker = req.worker;

    void *saveContext = worker->getContext();
    worker->setContext(req.context);

    Traversal<ForceData> *traversal = req.traversal; 
    State *state = req.state;
    Node<ForceData> *firstChild = (Node<ForceData>*) data;
    // start the traversal for each of the received children
    for(int j = 0; j < BRANCH_FACTOR; j++){
      traversal->topDownTraversal(firstChild+j,worker,state);
    }

    worker->setContext(saveContext);
    if(state->decrPending()){
      worker->done();
    }
  }

  requestors.clear();

}

#include "Traversal_defs.h"
