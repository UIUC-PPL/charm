#include <mpi.h> 
#include <cstdlib>
#include "fftw3-mpi.h"
#include <cmath>
#include <limits>
#include "fileio.h"

using namespace std;

int main(int argc, char *argv[]) {
  int rank, size; 
  MPI_Init(&argc, &argv);
  MPI_Comm_size(MPI_COMM_WORLD, &size);
  MPI_Comm_rank(MPI_COMM_WORLD, &rank); 

  fftw_plan plan;
  fftw_complex *data;

  fftw_mpi_init();

  if(rank == 0) {
    if(argc != 2) {
      printf("Usage: ./binary <N>\n");
      MPI_Abort(MPI_COMM_WORLD,-1);
    }
  }

  int N = atoi(argv[1]);

  ptrdiff_t local_ni=N*N/size, local_i_start = N*N/size*rank;
  ptrdiff_t local_no=local_ni, local_o_start = local_i_start;

  int b_or_f = FFTW_BACKWARD;

  ptrdiff_t alloc_local =  fftw_mpi_local_size_1d(N*N, MPI_COMM_WORLD,
      b_or_f, FFTW_ESTIMATE, &local_ni, &local_i_start,
      &local_no, &local_o_start);

  data = fftw_alloc_complex(alloc_local);

  plan = fftw_mpi_plan_dft_1d(N*N, data, data, MPI_COMM_WORLD, b_or_f, FFTW_ESTIMATE);

  char filename[80];
  sprintf(filename, "%d-%d.dump%d", size, N, rank);
  readCommFile(data, filename);

  fftw_execute(plan);

  double infNorm = 0.0;
  srand48(rank);
  for(int i = 0; i < N*N/size; i++) {
    data[i][0] = data[i][0]/(N*N) - drand48();
    data[i][1] = data[i][1]/(N*N) - drand48();

    double mag = sqrt(pow(data[i][0], 2) + pow(data[i][1], 2));
    if(mag > infNorm) infNorm = mag;
  }

  double my_r = infNorm / (std::numeric_limits<double>::epsilon() * log((double)N * N));
  double r;

  MPI_Reduce(&my_r, &r, 1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);

  if(rank == 0) {
    if(r < 16)
      printf("r = %g, PASS!\n",r);
    else
      printf("r = %g, FAIL\n",r);
  }

  fftw_destroy_plan(plan);

  fftw_mpi_cleanup();
  MPI_Finalize();      
  return 0; 
} 
