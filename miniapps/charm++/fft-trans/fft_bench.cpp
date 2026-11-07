//#include <stdio.h>
//#include <iostream>

#include <mpi.h>
#include <fftw3.h>
#include <cstdlib>
#include <limits>
#include <cmath>

using namespace std;

int main(int argc, char *argv[]) 
{
  MPI_Init(&argc, &argv);

  long N = atol(argv[1]);
  long n = N * N;

  fftw_complex *data;
  fftw_plan p, q;

  data = (fftw_complex*) fftw_malloc(sizeof(fftw_complex) * n);

  int length[] = {N};
  p = fftw_plan_many_dft(1, length, N, data, length, 1, N,
      data, length, 1, N, FFTW_FORWARD, FFTW_ESTIMATE); 
  q = fftw_plan_many_dft(1, length, N, data, length, 1, N,
      data, length, 1, N, FFTW_BACKWARD, FFTW_ESTIMATE);

  srand48(0);
  for(int i = 0; i<n; i++) {
    data[i][0] = drand48();
    data[i][1] = drand48();
  }

  double t1, t2; 
  t1 = MPI_Wtime(); 

  fftw_execute(p);

  t2 = MPI_Wtime() - t1; 
  printf( "On %ld elements, Elapsed time is %f with %f Gflop/s\n", n, t2, 5*(double)n*log2((double)n)/(t2*1000000000)); 

  fftw_execute(q);

  double infNorm = 0.0;

  srand48(0);
  for(int i=0; i<n; i++) {
    data[i][0] = data[i][0]/N - drand48();
    data[i][1] = data[i][1]/N - drand48();

    double mag = sqrt(pow(data[i][0],2) + pow(data[i][1], 2));
    if(mag > infNorm) infNorm = mag;
  } 

  double r = infNorm / (std::numeric_limits<double>::epsilon() * log((double)n));

  printf("residual = %f\n", r);

  fftw_destroy_plan(p);
  fftw_destroy_plan(q);
  fftw_free(data);

  MPI_Finalize();
  return 0;
}
