!***********************************************************************
! AMPI virtualization module for LUNs 12/30/2019
!                                                                      *
!***********************************************************************
!     initialize via AMPI_LUN_create()
!     call AMPI_LUN(LUN) to return the virtualized LUN with idempotency
!      
      module AMPI_LUN_Virtualized
      implicit none
      save
!     obtain AMPI rank
!     multiply by LUN factor, default 100000
      integer AMPI_LUN_thisrank
!$omp threadprivate(AMPI_LUN_thisrank)      
      integer :: AMPI_LUN_Factor=100000

      contains 

!-----------------------------------------------------------------------
      subroutine AMPI_LUN_create(thisRank)

      implicit none
      integer, intent(in) :: thisRank


      AMPI_LUN_thisrank = thisRank * AMPI_LUN_Factor

      end subroutine AMPI_LUN_create
!-----------------------------------------------------------------------

      integer function AMPI_LUN(inLUN) result(virt_LUN)
        implicit none
        integer, intent(in) :: inLUN

        virt_lun= xor(inLUN, AMPI_LUN_thisrank);
      
      end function AMPI_LUN

      end module  AMPI_LUN_Virtualized
