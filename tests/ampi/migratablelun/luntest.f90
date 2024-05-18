       module luntest
         use AMPI_LUN_Migratable
         implicit none
         include 'mpif.h'

      
       contains
       
         subroutine about_to_migrate
           implicit none

           integer :: rank, luncount, ierr

           call mpi_comm_rank(MPI_COMM_WORLD, rank, ierr)
!           write(*,*) rank, "About to migrate";
           luncount = AMPI_LUN_close_registered();
         end subroutine about_to_migrate

         subroutine just_migrated
           implicit none
                    
           integer :: rank, ierr
           call mpi_comm_rank(MPI_COMM_WORLD, rank, ierr)

           ierr= AMPI_LUN_reopen_registered();
!           write(*,*) rank, " Just migrated";
         end subroutine just_migrated



         subroutine openluns()
                    
           integer readlun, writelun
           integer registercount
           character(1024)::writefilename
           character(1024)::readfilename
           character(10):: writeaction
           character(10):: readaction
           character*5 rankstring
           integer rank, ierr, lun
           call mpi_comm_rank(MPI_COMM_WORLD, rank, ierr);
           writelun=(rank+1)+500;
           readlun=(rank+1)+100;
           write(rankstring,5) rank;
5          format(I4);
           readaction='READ';
           writeaction='WRITE';
           writefilename='output' // trim(adjustl(rankstring)) //'.out';
!           open(UNIT=writelun,FILE=writefilename,ACTION=writeaction);
           registercount= AMPI_LUN_open(writelun,writefilename,writeaction);
           readfilename='luntest.f90';
!           open(UNIT=readlun,FILE=readfilename,ACTION=readaction);
           registercount= AMPI_LUN_open(readlun,readfilename,readaction);
         end subroutine openluns

         function dowork(rank, iteration) result(operand)
           implicit none

           integer, intent(in)::rank, iteration
           integer work, ierr, range
           integer(8) operand
           operand=1;
           range=(rank+iteration);
           ! work scales with rank to create imbalance
           do work=1, range
              operand=work*operand;
           end do
           return
         end function dowork

         subroutine dooutput(iteration, invalue)
           implicit none

           integer, intent(in)::iteration
           integer(8), intent(in)::invalue
           integer rank, ierr, lun
           !output the current value to lun for our rank number
           call mpi_comm_rank(MPI_COMM_WORLD, rank, ierr);
           lun=(rank+1)+500;
           write(lun,*) invalue;
         end subroutine dooutput

         subroutine doinput(iteration)
           implicit none

           integer, intent(in)::iteration
           integer :: readlun, ioi
           character(100):: inchar
           integer rank, ierr, lun
           call mpi_comm_rank(MPI_COMM_WORLD, rank, ierr);
           readlun=(rank+1)+100;        
           ! we'll read a line

           read(readlun,*) inchar;

         end subroutine doinput

         function checkoutput(rank, iteration) result(stat)
           implicit none
           integer, intent(in)::rank
           integer, intent(in)::iteration
           integer checklun, ierr, checkiteration
           logical stat
           character*5 rankstring
           character(1024)::checkfilename
           integer(8) checkvalue
           integer(8) verifyvalue
           stat=.TRUE.;
           checklun=(rank+1)+200;
           write(rankstring,15) rank;
15         format(I4);
           checkfilename='output' // trim(adjustl(rankstring)) //'.out';
           open(UNIT=checklun,FILE=checkfilename,ACTION="READ");
           do checkiteration=1, iteration
              read(checklun,*) checkvalue;
              verifyvalue=dowork(rank,checkiteration);
              if(verifyvalue .ne. checkvalue) then
                 stat=.FALSE.;
!              else
!                 write (*,*) "checked that ",verifyvalue, " matches ", checkvalue;
              end if
           end do
           return;
         end function checkoutput

         subroutine migratablelun_test()
           implicit none

           integer :: iteration
           integer :: AMPI_LB_FREQ, CHECK_FREQ
           integer :: ierr, rank, numranks
           integer(8) :: computed
           !       create the lun registry
           call AMPI_LUN_create_registry(10);

           ! open some files and add them to the registry

           call openluns();

           !start iterating
           AMPI_LB_FREQ=5;
           CHECK_FREQ=18;
           call mpi_comm_rank(MPI_COMM_WORLD, rank, ierr);
           call MPI_Comm_size(MPI_COMM_WORLD, numranks, ierr)
                      
           do iteration=1, 19
              call doinput(iteration);
              computed=dowork(rank,iteration);
              call dooutput(iteration, computed);
              IF(MOD(iteration,AMPI_LB_FREQ).EQ.0)THEN
                 call AMPI_MIGRATE(AMPI_INFO_LB_SYNC, IERR);
              END IF
              IF (MOD(iteration,CHECK_FREQ).EQ.0) THEN
                 call MPI_BARRIER(MPI_COMM_WORLD, ierr);
                 IF(checkoutput(MOD(rank+1,numranks),iteration)) THEN
                    write (*,*) "rank ",rank," check of rank ", MOD(rank+1,numranks), " succeeded at iter", iteration;
                 ELSE
                    call MPI_ABORT(MPI_COMM_WORLD,1, ierr);
                 END IF
              END IF
           END DO
         end subroutine migratablelun_test
       end module luntest

