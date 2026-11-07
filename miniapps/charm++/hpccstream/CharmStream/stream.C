#include <iostream>
#include <cstdlib>
#include <stdlib.h>
#include <cmath>
#include <string>

using namespace std;

#include "stream.decl.h"

// N is per node array size, 24*N > 1/4 (system memory)
#define N 1000000
#define numIterations 10

CProxy_Controller controllerProxy;
double alpha;

class Controller:public CBase_Controller
{
		private:
				int numSteps; 
				CProxy_Worker workerProxy;
				double times[numIterations+1];
				bool validating;
		public:
				Controller(CkArgMsg *msg)
				{
							
						workerProxy = CProxy_Worker::ckNew();
						numSteps = 0;
						alpha = 3.0;
						controllerProxy = thisProxy;
						validating = false;

						CkCallback * cb = new CkCallback(CkIndex_Controller::StepComplete(NULL), thisProxy);
						workerProxy.ckSetReductionClient(cb);

						times[0] = CmiWallTimer();

						//call parallel Tried function
						workerProxy.Tried();

						delete msg;
				}

				Controller(CkMigrateMessage * msg){}
				
				void Done()
				{
					
						ckout<<"Verification Successful"<<endl;
						//Calculate min of times array and then calculate measured bandwidth
						double minTime=  times[0];
						for (int i =0; i < numIterations; i++)
								minTime = (minTime <= times[i])? minTime:times[i];
					
						ckout<<"*****************  Benchmark Finished  *****************"<<endl;
					//	ckout<<"Time (s) = "<<minTime <<endl;
						ckout<<" Performance (Gbytes/s) = "<< ((24 * N )/(minTime * 1000000000))<<endl;
						CkExit();
				}
							
				void StepComplete(CkReductionMsg *msg)
				{
						delete msg;
						
						// Called back from successful validation
						if (validating)
								Done();

						// Otherwise Called back from computaion
						times[numSteps+1] = CmiWallTimer();
						times[numSteps] = times[numSteps+1] - times[numSteps];
						
						numSteps++;
						if(numSteps < numIterations)
								workerProxy.Tried();
						else 
							{
							//Verify the results 
							validating = true;
							workerProxy.Verify();
							}
				}
			
};

class Worker:public CBase_Worker
{
		private:
				double a[N],b[N],c[N];

		public:
				Worker()
				{
						//Intiialize using random numbers
					   srand(CkMyPe());
					   for (int j=0; j<N; j++) {
							   b[j] = (rand()/(double)RAND_MAX+1);
							   c[j] = (rand()/(double)RAND_MAX+1);
							   }
					
				}

				/* Actual Tried Computation */
				void Tried()
				{
						for (int j=0; j<N; j++)
								a[j] = b[j] + alpha * c[j];

						contribute(); 
				}


				/* Verification */
				void Verify()
				{
						srand(CkMyPe());
						for(int j=0; j<N; j++)
								if (a[j] !=(rand()/(double)RAND_MAX+1) + alpha *  (rand()/(double)RAND_MAX+1))
								{
									ckout<<"Verification Failed"<<endl;
									CkExit();
								}
						contribute(); 
				}

				Worker(CkMigrateMessage * msg){}
};

#include "stream.def.h"



		

