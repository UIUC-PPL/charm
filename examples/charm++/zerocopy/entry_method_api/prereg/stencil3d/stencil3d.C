/** \file stencil3d.C

 *  This example is the nocopy verison of the stencil example
 *  in examples/charm++/load_balancing/stencil3d *
 *	      *****************
 *	   *		   *  *
 *   ^	*****************     *
 *   |	*		*     *
 *   |	*		*     *
 *   |	*		*     *
 *   Y	*		*     *
 *   |	*		*     *
 *   |	*		*     *
 *   |	*		*  *
 *   ~	*****************    Z
 *	<------ X ------>
 *
 *   X: left, right --> wrap_x
 *   Y: top, bottom --> wrap_y
 *   Z: front, back --> wrap_z
 */

#include "stencil3d.decl.h"
#include "TopoManager.h"

/*readonly*/ CProxy_Main mainProxy;
/*readonly*/ int arrayDimX;
/*readonly*/ int arrayDimY;
/*readonly*/ int arrayDimZ;
/*readonly*/ int blockDimX;
/*readonly*/ int blockDimY;
/*readonly*/ int blockDimZ;

// specify the number of worker chares in each dimension
/*readonly*/ int num_chare_x;
/*readonly*/ int num_chare_y;
/*readonly*/ int num_chare_z;

static unsigned long next = 1;

int myrand(int numpes) {
  next = next * 1103515245 + 12345;
  return((unsigned)(next/65536) % numpes);
}

// We want to wrap entries around, and because mod operator %
// sometimes misbehaves on negative values. -1 maps to the highest value.
#define wrap_x(a)	(((a)+num_chare_x)%num_chare_x)
#define wrap_y(a)	(((a)+num_chare_y)%num_chare_y)
#define wrap_z(a)	(((a)+num_chare_z)%num_chare_z)

#define index(a,b,c)	((a)+(b)*(blockDimX+2)+(c)*(blockDimX+2)*(blockDimY+2))

#define MAX_ITER	40
#define LBPERIOD_ITER	5    // LB is called every LBPERIOD_ITER number of program iterations
#define CHANGELOAD	30
#define LEFT		1
#define RIGHT		2
#define TOP		3
#define BOTTOM		4
#define FRONT		5
#define BACK		6
#define DIVIDEBY7      	0.14285714285714285714

/** \class Main
 *
 */
class Main : public CBase_Main {
  public:
    CProxy_Stencil array;

    Main(CkArgMsg* m) {
      if ( (m->argc != 3) && (m->argc != 7) ) {
        CkPrintf("%s [array_size] [block_size]\n", m->argv[0]);
        CkPrintf("OR %s [array_size_X] [array_size_Y] [array_size_Z] [block_size_X] [block_size_Y] [block_size_Z]\n", m->argv[0]);
        CkAbort("Abort");
      }

      // store the main proxy
      mainProxy = thisProxy;

      if(m->argc == 3) {
        arrayDimX = arrayDimY = arrayDimZ = atoi(m->argv[1]);
        blockDimX = blockDimY = blockDimZ = atoi(m->argv[2]);
      }
      else if (m->argc == 7) {
        arrayDimX = atoi(m->argv[1]);
        arrayDimY = atoi(m->argv[2]);
        arrayDimZ = atoi(m->argv[3]);
        blockDimX = atoi(m->argv[4]);
        blockDimY = atoi(m->argv[5]);
        blockDimZ = atoi(m->argv[6]);
      }

      if (arrayDimX < blockDimX || arrayDimX % blockDimX != 0)
        CkAbort("array_size_X %% block_size_X != 0!");
      if (arrayDimY < blockDimY || arrayDimY % blockDimY != 0)
        CkAbort("array_size_Y %% block_size_Y != 0!");
      if (arrayDimZ < blockDimZ || arrayDimZ % blockDimZ != 0)
        CkAbort("array_size_Z %% block_size_Z != 0!");

      num_chare_x = arrayDimX / blockDimX;
      num_chare_y = arrayDimY / blockDimY;
      num_chare_z = arrayDimZ / blockDimZ;

      // print info
      CkPrintf("\nSTENCIL COMPUTATION WITH BARRIERS\n");
      CkPrintf("Running Stencil on %d processors with (%d, %d, %d) chares\n", CkNumPes(), num_chare_x, num_chare_y, num_chare_z);
      CkPrintf("Array Dimensions: %d %d %d\n", arrayDimX, arrayDimY, arrayDimZ);
      CkPrintf("Block Dimensions: %d %d %d\n", blockDimX, blockDimY, blockDimZ);

      // Create new array of worker chares
      array = CProxy_Stencil::ckNew(num_chare_x, num_chare_y, num_chare_z);

      //Start the computation
      array.doStep();
    }

    // Each worker reports back to here when it completes an iteration
    void report() {
      CkExit();
    }
};

/** \class Stencil
 *
 */

class Stencil: public CBase_Stencil {
  Stencil_SDAG_CODE
  private:
    double startTime;

  public:
    int iterations;
    int imsg;
    int counter;

    double *temperature;
    double *new_temperature;
    CkCallback cb;

    // callback function called on completion of sending ghosts
    void completedSendingGhost(CkDataMsg *msg){
      CkNcpyBuffer *src = (CkNcpyBuffer *)(msg->data);
      void *ptr = (void *)(src->ptr);
      CkRdmaFree(ptr);
      delete msg;
      counter++;
      // Advance to next step on completion of sending ghosts to the 6 neighbors
      if(counter == 6){
        counter = 0;
        thisProxy[thisIndex].nextStep();
      }
    }

    // Constructor, initialize values
    Stencil() {
      usesAtSync = true;
      counter = 0;

      int i, j, k;
      cb = CkCallback(CkIndex_Stencil::completedSendingGhost(NULL), thisProxy(thisIndex.x, thisIndex.y, thisIndex.z));
      // allocate a three dimensional array
      temperature = new double[(blockDimX+2) * (blockDimY+2) * (blockDimZ+2)];
      new_temperature = new double[(blockDimX+2) * (blockDimY+2) * (blockDimZ+2)];

      for(k=0; k<blockDimZ+2; ++k)
        for(j=0; j<blockDimY+2; ++j)
          for(i=0; i<blockDimX+2; ++i)
            temperature[index(i, j, k)] = 0.0;

      iterations = 0;
      imsg = 0;
      constrainBC();
      // start measuring time
      if (thisIndex.x == 0 && thisIndex.y == 0 && thisIndex.z == 0)
        startTime = CkWallTimer();
    }

    void pup(PUP::er &p)
    {
      p|startTime;
      p|iterations;
      p|imsg;

      size_t size = (blockDimX+2) * (blockDimY+2) * (blockDimZ+2);
      if (p.isUnpacking()) {
        cb = CkCallback(CkIndex_Stencil::completedSendingGhost(NULL), thisProxy(thisIndex.x, thisIndex.y, thisIndex.z));
        temperature = new double[size];
        new_temperature = new double[size];
        counter = 0;
      }
      p(temperature, size);
      p(new_temperature, size);
    }

    Stencil(CkMigrateMessage* m) { }

    ~Stencil() {
      delete [] temperature;
      delete [] new_temperature;
    }

    // Send ghost faces to the six neighbors
    void begin_iteration(void) {
      iterations++;

      // Copy different faces into messages
      double *leftGhost =  (double *)CkRdmaAlloc(sizeof(double) * blockDimY * blockDimZ);
      double *rightGhost = (double *)CkRdmaAlloc(sizeof(double) * blockDimY * blockDimZ);
      double *topGhost =  (double *)CkRdmaAlloc(sizeof(double) * blockDimX * blockDimZ);
      double *bottomGhost = (double *)CkRdmaAlloc(sizeof(double) * blockDimX * blockDimZ);
      double *frontGhost =  (double *)CkRdmaAlloc(sizeof(double) * blockDimX * blockDimY);
      double *backGhost =  (double *)CkRdmaAlloc(sizeof(double) * blockDimX * blockDimY);

      for(int k=0; k<blockDimZ; ++k)
        for(int j=0; j<blockDimY; ++j) {
          leftGhost[k*blockDimY+j] = temperature[index(1, j+1, k+1)];
          rightGhost[k*blockDimY+j] = temperature[index(blockDimX, j+1, k+1)];
        }

      for(int k=0; k<blockDimZ; ++k)
        for(int i=0; i<blockDimX; ++i) {
          topGhost[k*blockDimX+i] = temperature[index(i+1, 1, k+1)];
          bottomGhost[k*blockDimX+i] = temperature[index(i+1, blockDimY, k+1)];
        }

      for(int j=0; j<blockDimY; ++j)
        for(int i=0; i<blockDimX; ++i) {
          frontGhost[j*blockDimX+i] = temperature[index(i+1, j+1, 1)];
          backGhost[j*blockDimX+i] = temperature[index(i+1, j+1, blockDimZ)];
        }

      // Send my left face
      thisProxy(wrap_x(thisIndex.x-1), thisIndex.y, thisIndex.z)
        .receiveGhosts(iterations, RIGHT, blockDimY, blockDimZ, CkSendBuffer(leftGhost, cb, CK_BUFFER_PREREG));
      // Send my right face
      thisProxy(wrap_x(thisIndex.x+1), thisIndex.y, thisIndex.z)
        .receiveGhosts(iterations, LEFT, blockDimY, blockDimZ, CkSendBuffer(rightGhost, cb, CK_BUFFER_PREREG));
      // Send my bottom face
      thisProxy(thisIndex.x, wrap_y(thisIndex.y-1), thisIndex.z)
        .receiveGhosts(iterations, TOP, blockDimX, blockDimZ, CkSendBuffer(bottomGhost, cb, CK_BUFFER_PREREG));
      // Send my top face
      thisProxy(thisIndex.x, wrap_y(thisIndex.y+1), thisIndex.z)
        .receiveGhosts(iterations, BOTTOM, blockDimX, blockDimZ, CkSendBuffer(topGhost, cb, CK_BUFFER_PREREG));
      // Send my front face
      thisProxy(thisIndex.x, thisIndex.y, wrap_z(thisIndex.z-1))
        .receiveGhosts(iterations, BACK, blockDimX, blockDimY, CkSendBuffer(frontGhost, cb, CK_BUFFER_PREREG));
      // Send my back face
      thisProxy(thisIndex.x, thisIndex.y, wrap_z(thisIndex.z+1))
        .receiveGhosts(iterations, FRONT, blockDimX, blockDimY, CkSendBuffer(backGhost, cb, CK_BUFFER_PREREG));

      // control flow continues in completedSendingGhost
    }

    void processGhosts(int dir, int height, int width, double gh[]) {
      switch(dir) {
        case LEFT:
          for(int k=0; k<width; ++k)
            for(int j=0; j<height; ++j) {
              temperature[index(0, j+1, k+1)] = gh[k*height+j];
            }
          break;
        case RIGHT:
          for(int k=0; k<width; ++k)
            for(int j=0; j<height; ++j) {
              temperature[index(blockDimX+1, j+1, k+1)] = gh[k*height+j];
            }
          break;
        case BOTTOM:
          for(int k=0; k<width; ++k)
            for(int i=0; i<height; ++i) {
              temperature[index(i+1, 0, k+1)] = gh[k*height+i];
            }
          break;
        case TOP:
          for(int k=0; k<width; ++k)
            for(int i=0; i<height; ++i) {
              temperature[index(i+1, blockDimY+1, k+1)] = gh[k*height+i];
            }
          break;
        case FRONT:
          for(int j=0; j<width; ++j)
            for(int i=0; i<height; ++i) {
              temperature[index(i+1, j+1, 0)] = gh[j*height+i];
            }
          break;
        case BACK:
          for(int j=0; j<width; ++j)
            for(int i=0; i<height; ++i) {
              temperature[index(i+1, j+1, blockDimZ+1)] = gh[j*height+i];
            }
          break;
        default:
          CkAbort("ERROR\n");
      }
    }

    void check_and_compute() {
      compute_kernel();

      // calculate error
      // not being done right now since we are doing a fixed no. of iterations

      double *tmp;
      tmp = temperature;
      temperature = new_temperature;
      new_temperature = tmp;

      constrainBC();

      if(thisIndex.x == 0 && thisIndex.y == 0 && thisIndex.z == 0) {
        double endTime = CkWallTimer();
        CkPrintf("[%d] Time per iteration: %f %f\n", iterations, (endTime - startTime), endTime);
      }

      if(iterations == MAX_ITER)
        contribute(CkCallback(CkReductionTarget(Main, report), mainProxy));
      else {
        if(thisIndex.x == 0 && thisIndex.y == 0 && thisIndex.z == 0)
          startTime = CkWallTimer();
        if(iterations % LBPERIOD_ITER == 0)
        {
          AtSync();
        }
        else {
          contribute(CkCallback(CkReductionTarget(Stencil, doStep), thisProxy));
        }
      }
    }

    // Check to see if we have received all neighbor values yet
    // If all neighbor values have been received, we update our values and proceed
    void compute_kernel() {
      int itno = (int)ceil((double)iterations/(double)CHANGELOAD) * 5;
      int index = thisIndex.x + thisIndex.y*num_chare_x + thisIndex.z*num_chare_x*num_chare_y;
      int numChares = num_chare_x * num_chare_y * num_chare_z;
      double work = 100.0;

      if(index >= numChares*0.2 && index <=numChares*0.8) {
        work = work * ((double)index/(double)numChares) + (double)itno;
      } else
        work = 10.0;

      for(int w=0; w<work; w++) {
        for(int k=1; k<blockDimZ+1; ++k)
          for(int j=1; j<blockDimY+1; ++j)
            for(int i=1; i<blockDimX+1; ++i) {
              // update my value based on the surrounding values
              new_temperature[index(i, j, k)] = (temperature[index(i-1, j, k)]
                  +  temperature[index(i+1, j, k)]
                  +  temperature[index(i, j-1, k)]
                  +  temperature[index(i, j+1, k)]
                  +  temperature[index(i, j, k-1)]
                  +  temperature[index(i, j, k+1)]
                  +  temperature[index(i, j, k)] )
                *  DIVIDEBY7;
            } // end for
      }
    }

    // Enforce some boundary conditions
    void constrainBC() {
      // Heat left, top and front faces of each chare's block
      for(int k=1; k<blockDimZ+1; ++k)
        for(int i=1; i<blockDimX+1; ++i)
          temperature[index(i, 1, k)] = 255.0;
      for(int k=1; k<blockDimZ+1; ++k)
        for(int j=1; j<blockDimY+1; ++j)
          temperature[index(1, j, k)] = 255.0;
      for(int j=1; j<blockDimY+1; ++j)
        for(int i=1; i<blockDimX+1; ++i)
          temperature[index(i, j, 1)] = 255.0;
    }

    void ResumeFromSync() {
      doStep();
    }
};

#include "stencil3d.def.h"
