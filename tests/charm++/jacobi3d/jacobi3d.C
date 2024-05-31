/** \file jacobi3d.C
 *  Author: Abhinav S Bhatele
 *  Date Created: June 01st, 2009
 *
 *  This does a topological placement for a 3d jacobi.
 *
 *	
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

#include "jacobi3d.decl.h"
#include <vector>
#include <utility>

#define CKP_FREQ    100
#define MAX_ITER    500
#define PRINT_FREQ  10
#define OVERHEAD 1024

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

/*readonly*/ int globalBarrier;
/*readonly*/ int maxIter;
/*readonly*/ int ckptFreq;

/*readonly*/ int overhead;

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

#define index(a, b, c)	((a)*(blockDimY+2)*(blockDimZ+2) + (b)*(blockDimZ+2) + (c))

#define WARM_ITER		5
#define LEFT			1
#define RIGHT			2
#define TOP			3
#define BOTTOM			4
#define FRONT			5
#define BACK			6
#define DIVIDEBY7       	0.14285714285714285714

double startTime;
double endTime;

/** \class ghostMsg
 *
 */
class ghostMsg: public CMessage_ghostMsg {
  public:
    int dir;
    int height;
    int width;
    double* gh;

    ghostMsg(int _d, int _h, int _w) : dir(_d), height(_h), width(_w) {
    }
};

/** \class Main
 *
 */
class Main : public CBase_Main {
  public:
    CProxy_Jacobi array;
    int iterations;
	std::vector<std::pair<double,int> > times;

    Main(CkArgMsg* m) {
      if ( (m->argc != 3) && (m->argc != 5) && (m->argc != 7) && (m->argc != 8) && (m->argc != 9) && (m->argc != 10) ) {
        CkPrintf("%s [array_size] [block_size]\n", m->argv[0]);
        CkPrintf("OR %s [array_size_X] [array_size_Y] [array_size_Z] [block_size_X] [block_size_Y] [block_size_Z] [msg_overhead_size]\n", m->argv[0]);
        CkPrintf("OR %s [array_size_X] [array_size_Y] [array_size_Z] [block_size_X] [block_size_Y] [block_size_Z] [max_iterations] [checkpoint_frequency]\n", m->argv[0]);
        CkPrintf("OR %s [array_size_X] [array_size_Y] [array_size_Z] [block_size_X] [block_size_Y] [block_size_Z] [max_iterations] [checkpoint_frequency] [msg_overhead_size] \n", m->argv[0]);
        CkAbort("Abort");
      }

      // set iteration counter to zero
      iterations = 0;
      overhead = OVERHEAD;

      // store the main proxy
	mainProxy = thisProxy;
	maxIter = MAX_ITER;
	ckptFreq = CKP_FREQ;
	
	if(m->argc == 3) {
		arrayDimX = arrayDimY = arrayDimZ = atoi(m->argv[1]);
		blockDimX = blockDimY = blockDimZ = atoi(m->argv[2]); 
	} else if (m->argc == 5) {
		arrayDimX = arrayDimY = arrayDimZ = atoi(m->argv[1]);
		blockDimX = blockDimY = blockDimZ = atoi(m->argv[2]); 
		maxIter = atoi(m->argv[3]);
		ckptFreq = atoi(m->argv[4]); 
	} else if (m->argc == 7) {
		arrayDimX = atoi(m->argv[1]);
		arrayDimY = atoi(m->argv[2]);
		arrayDimZ = atoi(m->argv[3]);
		blockDimX = atoi(m->argv[4]); 
		blockDimY = atoi(m->argv[5]); 
		blockDimZ = atoi(m->argv[6]);
	} else if (m->argc == 8) {
                arrayDimX = atoi(m->argv[1]);
                arrayDimY = atoi(m->argv[2]);
                arrayDimZ = atoi(m->argv[3]);
                blockDimX = atoi(m->argv[4]);
                blockDimY = atoi(m->argv[5]);
                blockDimZ = atoi(m->argv[6]);
                overhead = atoi(m->argv[8]);
        } else if (m->argc == 9) {
		arrayDimX = atoi(m->argv[1]);
		arrayDimY = atoi(m->argv[2]);
		arrayDimZ = atoi(m->argv[3]);
		blockDimX = atoi(m->argv[4]); 
		blockDimY = atoi(m->argv[5]); 
		blockDimZ = atoi(m->argv[6]);
		maxIter = atoi(m->argv[7]);
		ckptFreq = atoi(m->argv[8]);
	} else if (m->argc == 10) {
                arrayDimX = atoi(m->argv[1]);
                arrayDimY = atoi(m->argv[2]);
                arrayDimZ = atoi(m->argv[3]);
                blockDimX = atoi(m->argv[4]);
                blockDimY = atoi(m->argv[5]);
                blockDimZ = atoi(m->argv[6]);
                maxIter = atoi(m->argv[7]);
                ckptFreq = atoi(m->argv[8]);
                overhead = atoi(m->argv[9]);
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
      CkPrintf("Running Jacobi on %d processors with (%d, %d, %d) chares\n", CkNumPes(), num_chare_x, num_chare_y, num_chare_z);
      CkPrintf("Array Dimensions: %d %d %d\n", arrayDimX, arrayDimY, arrayDimZ);
      CkPrintf("Block Dimensions: %d %d %d\n", blockDimX, blockDimY, blockDimZ);

      // Create new array of worker chares
      array = CProxy_Jacobi::ckNew(num_chare_x, num_chare_y, num_chare_z);

      CkArray *jarr = array.ckLocalBranch();
      std::vector<std::vector<std::vector<int>>> jmap(num_chare_x, std::vector<std::vector<int> >(num_chare_y,std::vector <int>(num_chare_z)));

      int hops=0, p;
      for(int i=0; i<num_chare_x; i++)
	for(int j=0; j<num_chare_y; j++)
	  for(int k=0; k<num_chare_z; k++) {
	    jmap[i][j][k] = jarr->procNum(CkArrayIndex3D(i, j, k));
	  }

		//Start the computation
		startTime = CmiWallTimer();

		array.doStep();
    }

    // Each worker reports back to here when it completes an iteration
	void report(CkReductionMsg *msg) {
		int *value = (int *)msg->getData();
    	iterations = value[0];
		if (iterations < maxIter) {
			times.push_back(std::make_pair(CmiWallTimer() - startTime,iterations));
#ifdef CMK_MEM_CHECKPOINT
			if(iterations != 0 && iterations % ckptFreq == 0){
				CkCallback cb (CkIndex_Jacobi::doStep(), array);
				CkStartMemCheckpoint(cb);		
			}else{
				array.doStep();
			}
#else
			array.doStep();
#endif
      	} else {
			CkPrintf("Completed %d iterations\n", maxIter-1);
			endTime = CmiWallTimer();
//			CkPrintf("Time elapsed per iteration: %f\n", (endTime - startTime)/(MAX_ITER-1));
//			for(int i = 1; i < times.size(); i++)
//				CkPrintf("time=%.2f it=%d\n",times[i].first,times[i].second);
			CkExit();
		}
	}

};

/** \class Jacobi
 *
 */

class Jacobi: public CBase_Jacobi {
  // Jacobi_SDAG_CODE

  public:
    int iterations;
    int imsg;

    double *temperature;
    double *new_temperature;

    int msg_overhead_size;

    // Constructor, initialize values
    Jacobi() {

    	int i, j, k;
    	
        // allocate a three dimensional array
        temperature = new double[(blockDimX+2) * (blockDimY+2) * (blockDimZ+2)];
    	new_temperature = new double[(blockDimX+2) * (blockDimY+2) * (blockDimZ+2)];


		for(i=0; i<blockDimX+2; ++i) {
			for(j=0; j<blockDimY+2; ++j) {
	  			for(k=0; k<blockDimZ+2; ++k) {
	    			temperature[index(i, j, k)] = 0.0;
	  			}
			} 
      	}

		iterations = 0;
      	imsg = 0;
      	constrainBC();
        msg_overhead_size = overhead;
        usesAtSync = true;
    }

    Jacobi(CkMigrateMessage* m): CBase_Jacobi(m) {}

    ~Jacobi() { 
      delete [] temperature; 
      delete [] new_temperature; 
    }


	// Pupping function for migration and fault tolerance
	// Condition: assuming the 3D Chare Arrays are NOT used
	void pup(PUP::er &p){
          // pupping properties of this class
          p | iterations;
          p | imsg;
          p | msg_overhead_size;

          // if unpacking, allocate the memory space
          if(p.isUnpacking()){
            temperature = new double[(blockDimX+2) * (blockDimY+2) * (blockDimZ+2)];
            new_temperature = new double[(blockDimX+2) * (blockDimY+2) * (blockDimZ+2)];
          }

          // pupping the arrays
          p((char *)temperature, (blockDimX+2) * (blockDimY+2) * (blockDimZ+2) * sizeof(double));
          //p((char *) new_temperature, (blockDimX+2) * (blockDimY+2) * (blockDimZ+2) * sizeof(double));
	}

    // Send ghost faces to the six neighbors
    void doStep(void) {
      if (thisIndex.x == 0 && thisIndex.y == 0 && thisIndex.z == 0 && iterations % PRINT_FREQ == 0) {
          CkPrintf("Start of iteration %d at %f\n", iterations,CmiWallTimer());
      }
      iterations++;
	imsg++;
		

      // Copy different faces into messages
      ghostMsg *leftMsg = new (blockDimY*blockDimZ) ghostMsg(RIGHT, blockDimY, blockDimZ);
      ghostMsg *rightMsg = new (blockDimY*blockDimZ) ghostMsg(LEFT, blockDimY, blockDimZ);
      ghostMsg *topMsg = new (blockDimX*blockDimZ) ghostMsg(BOTTOM, blockDimX, blockDimZ);
      ghostMsg *bottomMsg = new (blockDimX*blockDimZ) ghostMsg(TOP, blockDimX, blockDimZ);
      ghostMsg *frontMsg = new (blockDimX*blockDimY) ghostMsg(BACK, blockDimX, blockDimY);
      ghostMsg *backMsg = new (blockDimX*blockDimY) ghostMsg(FRONT, blockDimX, blockDimY);

		
	
      CkSetRefNum(leftMsg, iterations);
      CkSetRefNum(rightMsg, iterations);
      CkSetRefNum(topMsg, iterations);
      CkSetRefNum(bottomMsg, iterations);
      CkSetRefNum(frontMsg, iterations);
      CkSetRefNum(backMsg, iterations);


     for(int j=0; j<blockDimY; ++j) 
	for(int k=0; k<blockDimZ; ++k) {
	  leftMsg->gh[k*blockDimY+j] = temperature[index(1, j+1, k+1)];
	  rightMsg->gh[k*blockDimY+j] = temperature[index(blockDimX, j+1, k+1)];
      }

		
		

      for(int i=0; i<blockDimX; ++i) 
	for(int k=0; k<blockDimZ; ++k) {
	  topMsg->gh[k*blockDimX+i] = temperature[index(i+1, 1, k+1)];
	  bottomMsg->gh[k*blockDimX+i] = temperature[index(i+1, blockDimY, k+1)];
      }

		

      for(int i=0; i<blockDimX; ++i) 
	for(int j=0; j<blockDimY; ++j) {
	  frontMsg->gh[j*blockDimX+i] = temperature[index(i+1, j+1, 1)];
	  backMsg->gh[j*blockDimX+i] = temperature[index(i+1, j+1, blockDimZ)];
      }

		

      // Send my left face
      thisProxy(wrap_x(thisIndex.x-1), thisIndex.y, thisIndex.z).receiveGhosts(leftMsg);
      // Send my right face
      thisProxy(wrap_x(thisIndex.x+1), thisIndex.y, thisIndex.z).receiveGhosts(rightMsg);
      // Send my top face
      thisProxy(thisIndex.x, wrap_y(thisIndex.y-1), thisIndex.z).receiveGhosts(topMsg);
      // Send my bottom face
      thisProxy(thisIndex.x, wrap_y(thisIndex.y+1), thisIndex.z).receiveGhosts(bottomMsg);
      // Send my front face
      thisProxy(thisIndex.x, thisIndex.y, wrap_z(thisIndex.z-1)).receiveGhosts(frontMsg);
      // Send my back face
      thisProxy(thisIndex.x, thisIndex.y, wrap_z(thisIndex.z+1)).receiveGhosts(backMsg);
	
	 if(imsg == 7){
        imsg = 0;
        check_and_compute();
    }	

    }

    void receiveGhosts(ghostMsg *gmsg) {
      int height = gmsg->height;
      int width = gmsg->width;

//		CkPrintf("[%d] Receiving data %d %d from %d...\n",CkMyPe(), height, width, gmsg->dir);
		

      switch(gmsg->dir) {
	case LEFT:
	  for(int j=0; j<height; ++j) 
	    for(int k=0; k<width; ++k) {
	      temperature[index(0, j+1, k+1)] = gmsg->gh[k*height+j];
	  }
	  break;
	case RIGHT:
	  for(int j=0; j<height; ++j) 
	    for(int k=0; k<width; ++k) {
	      temperature[index(blockDimX+1, j+1, k+1)] = gmsg->gh[k*height+j];
	  }
	  break;
	case TOP:
	  for(int i=0; i<height; ++i) 
	    for(int k=0; k<width; ++k) {
	      temperature[index(i+1, 0, k+1)] = gmsg->gh[k*height+i];
	  }
	  break;
	case BOTTOM:
	  for(int i=0; i<height; ++i) 
	    for(int k=0; k<width; ++k) {
	      temperature[index(i+1, blockDimY+1, k+1)] = gmsg->gh[k*height+i];
	  }
	  break;
	case FRONT:
	  for(int i=0; i<height; ++i) 
	    for(int j=0; j<width; ++j) {
	      temperature[index(i+1, j+1, blockDimZ+1)] = gmsg->gh[j*height+i];
	  }
	  break;
	case BACK:
	  for(int i=0; i<height; ++i) 
	    for(int j=0; j<width; ++j) {
	      temperature[index(i+1, j+1, 0)] = gmsg->gh[j*height+i];
	  }
	  break;
	default:
          CkAbort("ERROR\n");
      } 

      delete gmsg;


		imsg++;
		if(imsg == 7){
			imsg = 0;
			check_and_compute();
		}
    }


	void check_and_compute() {
    	compute_kernel();

		// interchanging the two different arrays
		double *tmp;
      	tmp = temperature;
      	temperature = new_temperature;
      	new_temperature = tmp;

		constrainBC();
#ifdef CMK_MESSAGE_LOGGING
		if(iterations % ckptFreq == 0){
			AtSync();
		} else
#endif
    {
      CkCallback cb(CkIndex_Main::report(NULL), mainProxy);
			contribute(sizeof(int), &iterations, CkReduction::max_int, cb);
		}
	}

	void ResumeFromSync(){
    CkCallback cb(CkIndex_Main::report(NULL), mainProxy);
		contribute(sizeof(int), &iterations, CkReduction::max_int, cb);
	}

    // Check to see if we have received all neighbor values yet
    // If all neighbor values have been received, we update our values and proceed
	void compute_kernel() {

		for(int i=1; i<blockDimX+1; ++i) {
			for(int j=1; j<blockDimY+1; ++j) {
				for(int k=1; k<blockDimZ+1; ++k) {

	    			// update my value based on the surrounding values
	    			new_temperature[index(i, j, k)] = (temperature[index(i-1, j, k)] 
					    +  temperature[index(i+1, j, k)]
					    +  temperature[index(i, j-1, k)]
					    +  temperature[index(i, j+1, k)]
					    +  temperature[index(i, j, k-1)]
					    +  temperature[index(i, j, k+1)]
					    +  temperature[index(i, j, k)] ) * DIVIDEBY7; 
	  			}
			}
      	} 
    }

    // Enforce some boundary conditions
	void constrainBC() {
    	// Heat left, top and front faces of each chare's block
      	for(int i=1; i<blockDimX+1; ++i)
			for(int k=1; k<blockDimZ+1; ++k)
	  			temperature[index(i, 1, k)] = 255.0;
      	for(int j=1; j<blockDimY+1; ++j)
			for(int k=1; k<blockDimZ+1; ++k)
	  			temperature[index(1, j, k)] = 255.0;
      	for(int i=1; i<blockDimX+1; ++i)
			for(int j=1; j<blockDimY+1; ++j)
	  			temperature[index(i, j, 1)] = 255.0; 
	}

};

#include "jacobi3d.def.h"
