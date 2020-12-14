/*
 * Charm++ interface to Collision detection system
 * Orion Sky Lawlor, olawlor@acm.org, 2003/3/19
 */
#ifndef __UIUC_CHARM_COLLIDE_H
#define __UIUC_CHARM_COLLIDE_H

#include "charm++.h"
#include "collide_util.h"
struct collideStats; // forward declaration for collideStats
#include "collidecharm.decl.h"

/********************** collideClient *****************
  A place for the assembled Collision lists to go.
  Each voxel calls this "Collisions" method with their
  lists of Collisions.  Because there can be many voxels per
  processor, the canonical implementation does some kind
  of reduction over the voxels array--this way you know
  when all voxels have reported their Collisions.

  If you just want the Collisions collected onto one processor,
  use the serialCollisionClient interface below.
  */
class collideClient : public Group {
  public:
    virtual ~collideClient();
    virtual void collisions(ArrayElement *src,
        int step,CollisionList &colls) =0;
};

/********************** serialCollideClient *****************
  Reduces the Collision list down to processor 0.
  */
/// This client function is called on PE 0 with a Collision list
typedef void (*CollisionClientFn)(void *param,int nColl,Collision *colls);

/// Call this on processor 0 to build a Collision client that
///  just calls this serial routine on processor 0 with the final,
///  complete Collision list.
CkGroupID CollideSerialClient(CollisionClientFn clientFn,void *clientParam);

/// Call this on processor 0 to build a Collision client that
/// uses the passed callback as the reduction target for the final,
/// complete Collision list.
CkGroupID CollideSerialClient(CkCallback clientCb);

/****************** Collision Interface ******************/
typedef CkGroupID CollideHandle;

/// Create a collider group to contribute objects to.
///  Should be called on processor 0.
CollideHandle CollideCreate(const CollideGrid3d &gridMap,
    CkGroupID clientGroupID);

/// Register with this collider group. (Call on creation/arrival)
void CollideRegister(CollideHandle h,int chunkNo);
/// Unregister with this collider group. (Call on deletion)
void CollideUnregister(CollideHandle h,int chunkNo);

/// Send these objects off to be collided.
/// The results go the collisionClient group
/// registered at creation time.
void CollideBoxesPrio(CollideHandle h,int chunkNo,
    int nBox,const bbox3d *boxes,const int *prio=NULL);

#endif
