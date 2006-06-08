/** The shadow array class attached to a fem chunk to perform all communication
 *  Author: Nilesh Choudhury
 */

#ifndef __PARFUM_SA_H
#define __PARFUM_SA_H

#include "tcharm.h"
#include "charm++.h"
#include "ParFUM.h"
#include "idxl.h"
#include "ParFUM_SA.decl.h"

///This is a message which packs all the chunk indices together
class lockChunksMsg : public CMessage_lockChunksMsg {
 public:
  ///list of chunks
  int *chkList;
  ///number of chunks in the list
  int chkListSize;
  ///type of idxl list
  int idxlType;

 public:
  lockChunksMsg(int *c, int s, int type) {
    chkListSize = s;
    idxlType = type;
  }

  ~lockChunksMsg() {
    ///if(chkList!=NULL) delete chkList;
  }

  int *getChks() {
    return chkList;
  }

  int getSize() {
    return chkListSize;
  }

  int getType() {
    return idxlType;
  }
};

///The shadow array attached to a fem chunk to perform all communication
/** This is a shadow array that should be used by all operations
    which want to perform any operations on meshes and for that
    purpose want to communicate among chunks on different processors
 */
class ParFUMShadowArray : public CBase_ParFUMShadowArray {
 private:
  ///Total number of chunks
  int numChunks;
  ///Index of this chunk (the chunk this is attached to)
  int idx;
  ///The Tcharm pointer to set it even outside the thread..
  TCharm *tc;
  ///The proxy for the current Tcharm object
  CProxy_TCharm tproxy;
  ///cross-pointer to the fem mesh on this chunk
  FEM_Mesh *fmMesh;
  ///Deprecated: used to lock this chunk

 public:
  ///constructor
  ParFUMShadowArray(int s, int i);
  ///constructor for migration
  ParFUMShadowArray(CkMigrateMessage *m);
  ///destructor
  ~ParFUMShadowArray();

  ///Pup to transfer this object's data
  void pup(PUP::er &p);
  ///This function is overloaded, it is called on this object just after migration
  void ckJustMigrated(void);

  ///Get number of chunks
  int getNumChunks(){return numChunks;}
  ///Get the index of this chunk
  int getIdx(){return idx;}
  ///Get the pointer to the mesh object
  FEM_Mesh *getfmMesh(){return fmMesh;}

  ///Initialize the mesh pointer for this chunk
  void setFemMesh(FEMMeshMsg *m);

  ///Sort this list of numbers in increasing order
  void sort(int *chkList, int chkListSize);
  ///Translates the sharedChk and the idxlType to the idxl side
  FEM_Comm *FindIdxlSide(int idxlType);

  ///Lock the particular idxl list on the following chunks
  void IdxlLockChunks(int* chkList, int chkListSize, int idxlType);
  ///Calls the above function, it is just a remote interface
  void IdxlLockChunksRemote(lockChunksMsg *lm);
  ///Lock the particular idxl lists only on this chunk
  void IdxlLockChunksSecondary(int *chkList, int chkListSize, int idxlType);
  ///Calls the above function, it is just a remote interface
  void IdxlLockChunksSecondaryRemote(lockChunksMsg *lm);

  ///Unlock the particular idxl list on the following chunks
  void IdxlUnlockChunks(int* chkList, int chkListSize, int idxlType);
  ///Calls the above function, it is just a remote interface
  void IdxlUnlockChunksRemote(lockChunksMsg *lm);
  ///Lock the particular idxl lists only on this chunk
  void IdxlUnlockChunksSecondary(int *chkList, int chkListSize, int idxlType);
  ///Calls the above function, it is just a remote interface
  void IdxlUnlockChunksSecondaryRemote(lockChunksMsg *lm);

  ///Add this 'localId' to this idxl list and return the shared entry index
  int IdxlAddPrimary(int localId, int sharedChk, int idxlType);
  ///Add this 'sharedIdx' to this idxl list
  bool IdxlAddSecondary(int localId, int sharedChk, int sharedIdx, int idxlType);
  ///Remove this 'localId' from this idxl list and return the shared entry index
  int IdxlRemovePrimary(int localId, int sharedChk, int idxlType);
  ///Remove the entry in this idxl list at index 'sharedIdx'
  bool IdxlRemoveSecondary(int sharedChk, int sharedIdx, int idxlType);

  ///Lookup this 'localId' in this idxl list and return the shared entry index
  int IdxlLookUpPrimary(int localId, int sharedChk, int idxlType);
  ///Return the localIdx at this 'sharedIdx' on this idxl list
  int IdxlLookUpSecondary(int sharedChk, int sharedIdx, int idxlType);
};

#endif

