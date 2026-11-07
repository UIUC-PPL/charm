
//@HEADER
// ************************************************************************
// 
//               HPCCG: Simple Conjugate Gradient Benchmark Code
//                 Copyright (2006) Sandia Corporation
// 
// Under terms of Contract DE-AC04-94AL85000, there is a non-exclusive
// license for use of this work by or on behalf of the U.S. Government.
// 
// This library is free software; you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as
// published by the Free Software Foundation; either version 2.1 of the
// License, or (at your option) any later version.
//  
// This library is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// Lesser General Public License for more details.
//  
// You should have received a copy of the GNU Lesser General Public
// License along with this library; if not, write to the Free Software
// Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
// USA
// Questions? Contact Michael A. Heroux (maherou@sandia.gov) 
// 
// ************************************************************************
//@HEADER

// Changelog
//
// Version 0.3
// - Added timing of setup time for sparse MV
// - Corrected percentages reported for sparse MV with overhead
//
/////////////////////////////////////////////////////////////////////////

// Main routine of a program that reads a sparse matrix, right side
// vector, solution vector and initial guess from a file  in HPC
// format.  This program then calls the HPCCG conjugate gradient
// solver to solve the problem, and then prints results.

// Calling sequence:

// test_HPCCG linear_system_file

// Routines called:

// read_HPC_row - Reads in linear system

// mytimer - Timing routine (compile with -DWALL to get wall clock
//           times

// HPCCG - CG Solver

// compute_residual - Compares HPCCG solution to known solution.

#include <iostream>
using std::cout;
using std::cerr;
using std::endl;
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cassert>
#include <string>
#include <cmath>
#ifdef USING_MPI
#include <mpi.h> // If this routine is compiled with -DUSING_MPI
                 // then include mpi.h
#include "make_local_matrix.hpp" // Also include this function
#endif
#include "generate_matrix.hpp"
#include "read_HPC_row.hpp"
#include "mytimer.hpp"
#include "HPC_sparsemv.hpp"
#include "compute_residual.hpp"
#include "HPCCG.hpp"
#include "HPC_Sparse_Matrix.hpp"


#undef DEBUG
int main(int argc, char *argv[])
{
  HPC_Sparse_Matrix *A;
  double *x, *b, *xexact;
  double norm, d;
  int ierr = 0;
  int i, j;
  int ione = 1;
  double times[7];
  double t6 = 0.0;

#ifdef USING_MPI

  // Initialize MPI
  MPI_Init(&argc, &argv);
  int size, rank; // Number of MPI processes, My process ID
  MPI_Comm_size(MPI_COMM_WORLD, &size);
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);

  // I'm alive !!!

  if (size < 100) cout << "Process "<<rank<<" of "<<size<<" is alive." <<endl;

#else

  int size = 1; // Serial case (not using MPI)
  int rank = 0; 

#endif


#ifdef DEBUG
  if (rank==0)
   {
    int junk = 0;
    cout << "Press enter to continue"<< endl;
    cin >> junk;
   }

  MPI_Barrier(MPI_COMM_WORLD);
#endif


  if(argc != 2 && argc!=4) {
    if (rank==0)
      cerr << "Usage:" << endl
	   << "Mode 1: " << argv[0] << " nx ny nz" << endl
	   << "     where nx, ny and nz are the local sub-block dimensions, or" << endl
	   << "Mode 2: " << argv[0] << " HPC_data_file " << endl
	   << "     where HPC_data_file is a globally accessible file containing matrix data." << endl;
    exit(1);
  }
  
  if (argc==4) {
    int nx = atoi(argv[1]);
    int ny = atoi(argv[2]);
    int nz = atoi(argv[3]);
    generate_matrix(nx, ny, nz, &A, &x, &b, &xexact, size, rank);
  }
  else
    read_HPC_row(argv[1], &A, &x, &b, &xexact, size, rank);

#ifdef USING_MPI

  // Transform matrix indices from global to local values.
  // Define number of columns for the local matrix.

  t6 = mytimer(); make_local_matrix(A);  t6 = mytimer() - t6;
  times[6] = t6;

#endif

  double t1 = mytimer();   // Initialize it (if needed)
  int niters = 0;
  double normr = 0.0;
  int max_iter = 150;
  double tolerance = 0.0; // Set tolerance to zero to make all runs do max_iter iterations
  ierr = HPCCG( A, b, x, max_iter, tolerance, niters, normr, times);

	if (ierr) cerr << "Error in call to CG: " << ierr << ".\n" << endl;

#ifdef USING_MPI
      double t4 = times[4];
      double t4min = 0.0;
      double t4max = 0.0;
      double t4avg = 0.0;
      MPI_Allreduce(&t4, &t4min, 1, MPI_DOUBLE, MPI_MIN, MPI_COMM_WORLD);
      MPI_Allreduce(&t4, &t4max, 1, MPI_DOUBLE, MPI_MAX, MPI_COMM_WORLD);
      MPI_Allreduce(&t4, &t4avg, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
      t4avg = t4avg/((double) size);
#endif

  if (rank==0)  // Only PE 0 needs to compute and report timing results
    {
      cout << "Time spent in CG = " << times[0] << ".\n" << endl;
      double fniters = niters; 
      double fnrow = A->total_nrow; double fnnz = A->total_nnz;
      double fnops_ddot = fniters*4*fnrow;
      double fnops_waxpby = fniters*6*fnrow;
      double fnops_sparsemv = fniters*2*fnnz;
      double fnops = fnops_ddot+fnops_waxpby+fnops_sparsemv;

      cout << "Number of iterations = " << niters << ".\n" << endl;
      cout << "Final residual = " << normr << ".\n" << endl;
      cout << "********** Performance Summary (times in sec) ***********" << endl << endl;
      cout << "Total Time/FLOPS/MFLOPS               = "
	   << times[0] << "/" << fnops << "/"
           << fnops/times[0]/1.0E6 << "." << endl;
      cout << "DDOT  Time/FLOPS/MFLOPS               = " 
	   << times[1] << "/" << fnops_ddot << "/"
           << fnops_ddot/times[1]/1.0E6 << "." << endl;
#ifdef USING_MPI
      cout << "     Minimum DDOT MPI_Allreduce time (over all processors) = " << t4min << endl;
      cout << "     Maximum DDOT MPI_Allreduce time (over all processors) = " << t4max << endl;
      cout << "     Average DDOT MPI_Allreduce time (over all processors) = " << t4avg << endl;
#endif
      cout << "WAXPBY Time/FLOPS/MFLOPS              = " 
	   << times[2] << "/" << fnops_waxpby << "/"
           << fnops_waxpby/times[2]/1.0E6 << "." << endl;
      cout << "SPARSEMV Time/FLOPS/MFLOPS            = " 
	   << times[3] << "/" << fnops_sparsemv << "/"
           << fnops_sparsemv/(times[3])/1.0E6 << "." << endl;
#ifdef USING_MPI
      double totalSparseMVTime = times[3] + times[5]+ times[6];
      cout << "SPARSEMV MFLOPS W OVRHEAD             = " 
           << fnops_sparsemv/(totalSparseMVTime)/1.0E6 << "." << endl;
      cout << "SPARSEMV PARALLEL OVERHEAD Time       = " 
           << (times[5]+times[6]) << " ( " << (times[5]+times[6])/totalSparseMVTime*100.0 << " % )." << endl;
      cout << "     SPARSEMV PARALLEL OVERHEAD (Setup) Time         = " 
           << times[6] << " ( " << times[6]/totalSparseMVTime*100.0 << " % )." << endl;
      cout << "     SPARSEMV PARALLEL OVERHEAD (Bdry Exchange) Time = " 
           << times[5] << " ( " << times[5]/totalSparseMVTime*100.0 << " % )." << endl;
#endif
    }

  // Compute difference between known exact solution and computed solution
  // All processors are needed here.

  double residual = 0;
  if ((ierr = compute_residual(A->local_nrow, x, xexact, &residual)))
    cerr << "Error in call to compute_residual: " << ierr << ".\n" << endl;

  if (rank==0)
    cout << "Difference between computed and exact  = " 
         << residual << ".\n" << endl;

  // Finish up
#ifdef USING_MPI
  MPI_Finalize();
#endif
  return 0 ;
} 
