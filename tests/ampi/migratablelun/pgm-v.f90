subroutine MPI_Main
  use AMPI_LUN_Migratable
  use AMPI_LUN_Virtualized
  use virtluntest, only: about_to_migrate, just_migrated, virtmigratablelun_test


  implicit none
  include 'mpif.h'

  integer :: myrank, ierr, numranks

  call MPI_Init(ierr)

  call MPI_Comm_rank(MPI_COMM_WORLD, myrank, ierr)
  call MPI_Comm_size(MPI_COMM_WORLD, numranks, ierr)
  if (myrank.eq. 0) then
     print *,"Initialized";
  endif
  call ampi_register_about_to_migrate(about_to_migrate, ierr);
  call ampi_register_just_migrated(just_migrated, ierr);
  call virtmigratablelun_test();

  
  call MPI_Finalize(ierr)

end subroutine
