#include "ampi.h"

#include <string.h>
#include <stdio.h>
#include <vector>

FLINKAGE {
#define mpi_send FTN_NAME( MPI_SEND , mpi_send )
#define mpi_recv FTN_NAME( MPI_RECV , mpi_recv )
#define mpi_mrecv FTN_NAME( MPI_MRECV , mpi_mrecv )
#define mpi_get_count FTN_NAME( MPI_GET_COUNT , mpi_get_count )
#define mpi_isend FTN_NAME( MPI_ISEND , mpi_isend )
#define mpi_bsend FTN_NAME( MPI_BSEND , mpi_bsend )
#define mpi_rsend FTN_NAME( MPI_RSEND , mpi_rsend )
#define mpi_ibsend FTN_NAME( MPI_IBSEND , mpi_ibsend )
#define mpi_irsend FTN_NAME( MPI_IRSEND , mpi_irsend )
#define mpi_ssend FTN_NAME( MPI_SSEND , mpi_ssend )
#define mpi_issend FTN_NAME( MPI_ISSEND , mpi_issend )
#define mpi_irecv FTN_NAME( MPI_IRECV , mpi_irecv )
#define mpi_imrecv FTN_NAME( MPI_IMRECV , mpi_imrecv )
#define mpi_buffer_attach FTN_NAME( MPI_BUFFER_ATTACH , mpi_buffer_attach )
#define mpi_buffer_detach FTN_NAME( MPI_BUFFER_DETACH , mpi_buffer_detach )
#define mpi_wait FTN_NAME( MPI_WAIT , mpi_wait )
#define mpi_test FTN_NAME( MPI_TEST , mpi_test )
#define mpi_waitany FTN_NAME( MPI_WAITANY , mpi_waitany )
#define mpi_testany FTN_NAME( MPI_TESTANY , mpi_testany )
#define mpi_waitall FTN_NAME( MPI_WAITALL , mpi_waitall )
#define mpi_testall FTN_NAME( MPI_TESTALL , mpi_testall )
#define mpi_waitsome FTN_NAME( MPI_WAITSOME , mpi_waitsome )
#define mpi_testsome FTN_NAME( MPI_TESTSOME , mpi_testsome )
#define mpi_request_get_status FTN_NAME(MPI_REQUEST_GET_STATUS , mpi_request_get_status)
#define mpi_request_free FTN_NAME(MPI_REQUEST_FREE , mpi_request_free)
#define mpi_grequest_start FTN_NAME(MPI_GREQUEST_START , mpi_grequest_start)
#define mpi_grequest_complete FTN_NAME(MPI_GREQUEST_COMPLETE , mpi_grequest_complete)
#define mpi_cancel FTN_NAME(MPI_CANCEL, mpi_cancel)
#define mpi_test_cancelled FTN_NAME(MPI_TEST_CANCELLED, mpi_test_cancelled)
#define mpi_status_set_cancelled FTN_NAME( MPI_STATUS_SET_CANCELLED , mpi_status_set_cancelled )
#define mpi_iprobe FTN_NAME( MPI_IPROBE , mpi_iprobe )
#define mpi_probe FTN_NAME( MPI_PROBE , mpi_probe )
#define mpi_improbe FTN_NAME( MPI_IMPROBE , mpi_improbe )
#define mpi_mprobe FTN_NAME( MPI_MPROBE , mpi_mprobe )
#define mpi_send_init FTN_NAME( MPI_SEND_INIT , mpi_send_init )
#define mpi_rsend_init FTN_NAME( MPI_RSEND_INIT , mpi_rsend_init )
#define mpi_bsend_init FTN_NAME( MPI_BSEND_INIT , mpi_bsend_init )
#define mpi_recv_init FTN_NAME( MPI_RECV_INIT , mpi_recv_init )
#define mpi_start FTN_NAME( MPI_START , mpi_start )
#define mpi_startall FTN_NAME( MPI_STARTALL , mpi_startall )
#define mpi_sendrecv FTN_NAME( MPI_SENDRECV , mpi_sendrecv )
#define mpi_sendrecv_replace FTN_NAME( MPI_SENDRECV_REPLACE , mpi_sendrecv_replace )
#define mpi_type_match_size FTN_NAME( MPI_TYPE_MATCH_SIZE , mpi_type_match_size )
#define mpi_type_contiguous FTN_NAME( MPI_TYPE_CONTIGUOUS , mpi_type_contiguous )
#define mpi_type_vector FTN_NAME( MPI_TYPE_VECTOR , mpi_type_vector )
#define mpi_type_hvector FTN_NAME( MPI_TYPE_HVECTOR , mpi_type_hvector )
#define mpi_type_create_hvector FTN_NAME( MPI_TYPE_CREATE_HVECTOR , mpi_type_create_hvector )
#define mpi_type_indexed FTN_NAME( MPI_TYPE_INDEXED , mpi_type_indexed )
#define mpi_type_create_hindexed FTN_NAME( MPI_TYPE_CREATE_HINDEXED , mpi_type_create_hindexed )
#define mpi_type_hindexed FTN_NAME( MPI_TYPE_HINDEXED , mpi_type_hindexed )
#define mpi_type_create_indexed_block FTN_NAME( MPI_TYPE_CREATE_INDEXED_BLOCK , mpi_type_create_indexed_block )
#define mpi_type_create_hindexed_block FTN_NAME( MPI_TYPE_CREATE_HINDEXED_BLOCK , mpi_type_create_hindexed_block )
#define mpi_type_create_struct FTN_NAME( MPI_TYPE_CREATE_STRUCT , mpi_type_create_struct )
#define mpi_type_struct FTN_NAME( MPI_TYPE_STRUCT , mpi_type_struct )
#define mpi_type_commit FTN_NAME( MPI_TYPE_COMMIT , mpi_type_commit )
#define mpi_type_free FTN_NAME( MPI_TYPE_FREE , mpi_type_free )
#define mpi_type_create_darray FTN_NAME( MPI_TYPE_CREATE_DARRAY , mpi_type_create_darray )
#define mpi_type_create_subarray FTN_NAME( MPI_TYPE_CREATE_SUBARRAY , mpi_type_create_subarray )
#define mpi_type_get_extent FTN_NAME( MPI_TYPE_GET_EXTENT , mpi_type_get_extent )
#define mpi_type_get_extent_x FTN_NAME( MPI_TYPE_GET_EXTENT_X , mpi_type_get_extent_x )
#define mpi_type_extent FTN_NAME( MPI_TYPE_EXTENT , mpi_type_extent )
#define mpi_type_get_true_extent FTN_NAME( MPI_TYPE_GET_TRUE_EXTENT , mpi_get_true_extent )
#define mpi_type_get_true_extent_x FTN_NAME( MPI_TYPE_GET_TRUE_EXTENT_X , mpi_get_true_extent_x )
#define mpi_type_size FTN_NAME( MPI_TYPE_SIZE , mpi_type_size )
#define mpi_type_size_x FTN_NAME( MPI_TYPE_SIZE_X , mpi_type_size_x )
#define mpi_type_lb FTN_NAME( MPI_TYPE_LB , mpi_type_lb )
#define mpi_type_ub FTN_NAME( MPI_TYPE_UB , mpi_type_ub )
/* mpi_type_set_name is defined in ampifimpl.f90, see ampif_type_set_name defined below */
#define mpi_type_get_name FTN_NAME( MPI_TYPE_GET_NAME , mpi_type_get_name )
#define mpi_type_create_resized FTN_NAME( MPI_TYPE_CREATE_RESIZED, mpi_type_create_resized )
#define mpi_type_dup FTN_NAME( MPI_TYPE_DUP, mpi_type_dup )
#define mpi_type_set_attr FTN_NAME( MPI_TYPE_SET_ATTR, mpi_type_set_attr )
#define mpi_type_get_attr FTN_NAME( MPI_TYPE_GET_ATTR, mpi_type_get_attr )
#define mpi_type_delete_attr FTN_NAME( MPI_TYPE_DELETE_ATTR, mpi_type_delete_attr )
#define mpi_type_create_keyval FTN_NAME( MPI_TYPE_CREATE_KEYVAL, mpi_type_create_keyval )
#define mpi_type_free_keyval FTN_NAME( MPI_TYPE_FREE_KEYVAL, mpi_type_free_keyval )
#define mpi_get_address FTN_NAME( MPI_GET_ADDRESS , mpi_get_address )
#define mpi_address FTN_NAME( MPI_ADDRESS , mpi_address )
#define mpi_status_set_elements FTN_NAME( MPI_STATUS_SET_ELEMENTS , mpi_status_set_elements )
#define mpi_status_set_elements_x FTN_NAME( MPI_STATUS_SET_ELEMENTS_X , mpi_status_set_elements_x )
#define mpi_get_elements FTN_NAME( MPI_GET_ELEMENTS , mpi_get_elements )
#define mpi_get_elements_x FTN_NAME( MPI_GET_ELEMENTS_X , mpi_get_elements_x )
#define mpi_pack FTN_NAME( MPI_PACK , mpi_pack )
#define mpi_unpack FTN_NAME( MPI_UNPACK , mpi_unpack )
#define mpi_pack_size FTN_NAME( MPI_PACK_SIZE , mpi_pack_size )
#define mpi_aint_add FTN_NAME( MPI_AINT_ADD , mpi_aint_add )
#define mpi_aint_diff FTN_NAME( MPI_AINT_DIFF , mpi_aint_diff )

#define mpi_barrier FTN_NAME( MPI_BARRIER , mpi_barrier )
#define mpi_ibarrier FTN_NAME( MPI_IBARRIER , mpi_ibarrier )
#define mpi_bcast FTN_NAME( MPI_BCAST , mpi_bcast )
#define mpi_ibcast FTN_NAME( MPI_IBCAST , mpi_ibcast )
#define mpi_gather FTN_NAME( MPI_GATHER , mpi_gather )
#define mpi_igather FTN_NAME( MPI_IGATHER , mpi_igather )
#define mpi_gatherv FTN_NAME( MPI_GATHERV , mpi_gatherv )
#define mpi_igatherv FTN_NAME( MPI_IGATHERV , mpi_igatherv )
#define mpi_scatter FTN_NAME( MPI_SCATTER , mpi_scatter )
#define mpi_iscatter FTN_NAME( MPI_ISCATTER , mpi_iscatter )
#define mpi_scatterv FTN_NAME( MPI_SCATTERV , mpi_scatterv )
#define mpi_iscatterv FTN_NAME( MPI_ISCATTERV , mpi_iscatterv )
#define mpi_allgather FTN_NAME( MPI_ALLGATHER , mpi_allgather )
#define mpi_iallgather FTN_NAME( MPI_IALLGATHER , mpi_iallgather )
#define mpi_allgatherv FTN_NAME( MPI_ALLGATHERV , mpi_allgatherv )
#define mpi_iallgatherv FTN_NAME( MPI_IALLGATHERV , mpi_iallgatherv )
#define mpi_alltoall FTN_NAME( MPI_ALLTOALL , mpi_alltoall )
#define mpi_ialltoall FTN_NAME( MPI_IALLTOALL , mpi_ialltoall )
#define mpi_alltoallv FTN_NAME( MPI_ALLTOALLV , mpi_alltoallv )
#define mpi_ialltoallv FTN_NAME( MPI_IALLTOALLV , mpi_ialltoallv )
#define mpi_alltoallw FTN_NAME( MPI_ALLTOALLW , mpi_alltoallw )
#define mpi_ialltoallw FTN_NAME( MPI_IALLTOALLW , mpi_ialltoallw )
#define mpi_reduce FTN_NAME( MPI_REDUCE , mpi_reduce )
#define mpi_ireduce FTN_NAME( MPI_IREDUCE , mpi_ireduce )
#define mpi_allreduce FTN_NAME( MPI_ALLREDUCE , mpi_allreduce )
#define mpi_iallreduce FTN_NAME( MPI_IALLREDUCE , mpi_iallreduce )
#define mpi_reduce_local FTN_NAME( MPI_REDUCE_LOCAL , mpi_reduce_local )
#define mpi_reduce_scatter_block FTN_NAME( MPI_REDUCE_SCATTER_BLOCK , mpi_reduce_scatter_block )
#define mpi_ireduce_scatter_block FTN_NAME( MPI_IREDUCE_SCATTER_BLOCK , mpi_ireduce_scatter_block )
#define mpi_reduce_scatter FTN_NAME( MPI_REDUCE_SCATTER , mpi_reduce_scatter )
#define mpi_ireduce_scatter FTN_NAME( MPI_IREDUCE_SCATTER , mpi_ireduce_scatter )
#define mpi_scan FTN_NAME( MPI_SCAN , mpi_scan )
#define mpi_iscan FTN_NAME( MPI_ISCAN , mpi_iscan )
#define mpi_exscan FTN_NAME( MPI_EXSCAN , mpi_exscan )
#define mpi_iexscan FTN_NAME( MPI_IEXSCAN , mpi_iexscan )
#define mpi_neighbor_alltoall FTN_NAME( MPI_NEIGHBOR_ALLTOALL , mpi_neighbor_alltoall )
#define mpi_ineighbor_alltoall FTN_NAME( MPI_INEIGHBOR_ALLTOALL , mpi_ineighbor_alltoall )
#define mpi_neighbor_alltoallv FTN_NAME( MPI_NEIGHBOR_ALLTOALLV , mpi_neighbor_alltoallv )
#define mpi_ineighbor_alltoallv FTN_NAME( MPI_INEIGHBOR_ALLTOALLV , mpi_ineighbor_alltoallv )
#define mpi_neighbor_alltoallw FTN_NAME( MPI_NEIGHBOR_ALLTOALLW , mpi_neighbor_alltoallw )
#define mpi_ineighbor_alltoallw FTN_NAME( MPI_INEIGHBOR_ALLTOALLW , mpi_ineighbor_alltoallw )
#define mpi_neighbor_allgather FTN_NAME( MPI_NEIGHBOR_ALLGATHER , mpi_neighbor_allgather )
#define mpi_ineighbor_allgather FTN_NAME( MPI_INEIGHBOR_ALLGATHER , mpi_ineighbor_allgather )
#define mpi_neighbor_allgatherv FTN_NAME( MPI_NEIGHBOR_ALLGATHERV , mpi_neighbor_allgatherv )
#define mpi_ineighbor_allgatherv FTN_NAME( MPI_INEIGHBOR_ALLGATHERV , mpi_ineighbor_allgatherv )
#define mpi_op_create FTN_NAME( MPI_OP_CREATE , mpi_op_create )
#define mpi_op_free FTN_NAME( MPI_OP_FREE , mpi_op_free )
#define mpi_op_commutative FTN_NAME( MPI_OP_COMMUTATIVE , mpi_op_commutative )

#define mpi_group_size FTN_NAME( MPI_GROUP_SIZE, mpi_group_size)
#define mpi_group_rank FTN_NAME( MPI_GROUP_RANK, mpi_group_rank)
#define mpi_group_translate_ranks FTN_NAME(MPI_GROUP_TRANSLATE_RANKS, mpi_group_translate_ranks)
#define mpi_group_compare FTN_NAME(MPI_GROUP_COMPARE, mpi_group_compare)
#define mpi_comm_group FTN_NAME( MPI_COMM_GROUP, mpi_comm_group)
#define mpi_group_union FTN_NAME(MPI_GROUP_UNION, mpi_group_union)
#define mpi_group_intersection FTN_NAME(MPI_GROUP_INTERSECTION, mpi_group_intersection)
#define mpi_group_difference FTN_NAME(MPI_GROUP_DIFFERENCE, mpi_group_difference)
#define mpi_group_incl FTN_NAME(MPI_GROUP_INCL, mpi_group_incl)
#define mpi_group_excl FTN_NAME(MPI_GROUP_EXCL, mpi_group_excl)
#define mpi_group_range_incl FTN_NAME(MPI_GROUP_RANGE_INCL, mpi_group_range_incl)
#define mpi_group_range_excl FTN_NAME(MPI_GROUP_RANGE_EXCL, mpi_group_range_excl)
#define mpi_group_free FTN_NAME(MPI_GROUP_FREE, mpi_group_free)

#define mpi_intercomm_create FTN_NAME ( MPI_INTERCOMM_CREATE , mpi_intercomm_create )
#define mpi_intercomm_merge FTN_NAME ( MPI_INTERCOMM_MERGE , mpi_intercomm_merge )

#define mpi_comm_create FTN_NAME(MPI_COMM_CREATE, mpi_comm_create)
#define mpi_comm_create_group FTN_NAME(MPI_COMM_CREATE_GROUP, mpi_comm_create_group)
#define mpi_comm_rank FTN_NAME( MPI_COMM_RANK , mpi_comm_rank )
#define mpi_comm_size FTN_NAME( MPI_COMM_SIZE , mpi_comm_size )
#define mpi_comm_dup FTN_NAME( MPI_COMM_DUP , mpi_comm_dup )
#define mpi_comm_idup FTN_NAME( MPI_COMM_IDUP , mpi_comm_idup )
#define mpi_comm_dup_with_info FTN_NAME( MPI_COMM_DUP_WITH_INFO , mpi_comm_dup_with_info )
#define mpi_comm_idup_with_info FTN_NAME( MPI_COMM_IDUP_WITH_INFO , mpi_comm_idup_with_info )
#define mpi_comm_split FTN_NAME( MPI_COMM_SPLIT , mpi_comm_split )
#define mpi_comm_split_type FTN_NAME( MPI_COMM_SPLIT_TYPE , mpi_comm_split_type )
#define mpi_comm_free FTN_NAME( MPI_COMM_FREE , mpi_comm_free )
#define mpi_comm_test_inter FTN_NAME( MPI_COMM_TEST_INTER , mpi_comm_test_inter )
#define mpi_comm_remote_size FTN_NAME ( MPI_COMM_REMOTE_SIZE , mpi_comm_remote_size )
#define mpi_comm_remote_group FTN_NAME ( MPI_COMM_REMOTE_GROUP , mpi_comm_remote_group )
/* mpi_comm_set_name is defined in ampifimpl.f90, see ampif_comm_set_name defined below */
#define mpi_comm_get_name FTN_NAME ( MPI_COMM_GET_NAME , mpi_comm_get_name )
#define mpi_comm_set_info FTN_NAME ( MPI_COMM_SET_INFO , mpi_comm_set_info )
#define mpi_comm_get_info FTN_NAME ( MPI_COMM_GET_INFO , mpi_comm_get_info )
#define mpi_comm_call_errhandler FTN_NAME( MPI_COMM_CALL_ERRHANDLER , mpi_comm_call_errhandler )
#define mpi_comm_create_errhandler FTN_NAME( MPI_COMM_CREATE_ERRHANDLER , mpi_comm_create_errhandler )
#define mpi_comm_set_errhandler FTN_NAME( MPI_COMM_SET_ERRHANDLER , mpi_comm_set_errhandler )
#define mpi_comm_get_errhandler FTN_NAME( MPI_COMM_GET_ERRHANDLER , mpi_comm_get_errhandler )
#define mpi_comm_free_errhandler FTN_NAME( MPI_COMM_FREE_ERRHANDLER , mpi_comm_free_errhandler )
#define mpi_comm_create_keyval FTN_NAME ( MPI_COMM_CREATE_KEYVAL , mpi_comm_create_keyval )
#define mpi_comm_free_keyval FTN_NAME ( MPI_COMM_FREE_KEYVAL , mpi_comm_free_keyval )
#define mpi_comm_set_attr FTN_NAME ( MPI_COMM_SET_ATTR , mpi_comm_set_attr )
#define mpi_comm_get_attr FTN_NAME ( MPI_COMM_GET_ATTR , mpi_comm_get_attr )
#define mpi_comm_delete_attr FTN_NAME ( MPI_COMM_DELETE_ATTR , mpi_comm_delete_attr )

#define mpi_keyval_create FTN_NAME ( MPI_KEYVAL_CREATE , mpi_keyval_create )
#define mpi_keyval_free FTN_NAME ( MPI_KEYVAL_FREE , mpi_keyval_free )
#define mpi_attr_put FTN_NAME ( MPI_ATTR_PUT , mpi_attr_put )
#define mpi_attr_get FTN_NAME ( MPI_ATTR_GET , mpi_attr_get )
#define mpi_attr_delete FTN_NAME ( MPI_ATTR_DELETE , mpi_attr_delete )

#define mpi_cart_create FTN_NAME ( MPI_CART_CREATE , mpi_cart_create )
#define mpi_graph_create FTN_NAME ( MPI_GRAPH_CREATE , mpi_graph_create )
#define mpi_dist_graph_create_adjacent FTN_NAME ( MPI_DIST_GRAPH_CREATE_ADJACENT, mpi_dist_graph_create_adjacent )
#define mpi_dist_graph_create FTN_NAME ( MPI_DIST_GRAPH_CREATE, mpi_dist_graph_create )
#define mpi_topo_test FTN_NAME ( MPI_TOPO_TEST , mpi_topo_test )
#define mpi_cart_map FTN_NAME ( MPI_CART_MAP , mpi_cart_map )
#define mpi_graph_map FTN_NAME ( MPI_GRAPH_MAP , mpi_graph_map )
#define mpi_cartdim_get FTN_NAME ( MPI_CARTDIM_GET , mpi_cartdim_get )
#define mpi_cart_get FTN_NAME ( MPI_CART_GET , mpi_cart_get )
#define mpi_cart_rank FTN_NAME ( MPI_CART_RANK , mpi_cart_rank )
#define mpi_cart_coords FTN_NAME ( MPI_CART_COORDS , mpi_cart_coords )
#define mpi_cart_shift FTN_NAME ( MPI_CART_SHIFT , mpi_cart_shift )
#define mpi_graphdims_get FTN_NAME ( MPI_GRAPHDIMS_GET , mpi_graphdims_get )
#define mpi_graph_get FTN_NAME ( MPI_GRAPH_GET , mpi_graph_get )
#define mpi_graph_neighbors_count FTN_NAME ( MPI_GRAPH_NEIGHBORS_COUNT , mpi_graph_neighbors_count )
#define mpi_graph_neighbors FTN_NAME ( MPI_GRAPH_NEIGHBORS , mpi_graph_neighbors )
#define mpi_dist_graph_neighbors FTN_NAME ( MPI_DIST_GRAPH_NEIGHBORS , mpi_dist_graph_neighbors )
#define mpi_dist_graph_neighbors_count FTN_NAME ( MPI_DIST_GRAPH_NEIGHBORS_COUNT , mpi_dist_graph_neighbors_count )
#define mpi_dims_create FTN_NAME ( MPI_DIMS_CREATE , mpi_dims_create )
#define mpi_cart_sub FTN_NAME ( MPI_CART_SUB , mpi_cart_sub )

#define mpi_get_version FTN_NAME ( MPI_GET_VERSION , mpi_get_version )
#define mpi_get_library_version FTN_NAME ( MPI_GET_LIBRARY_VERSION , mpi_get_library_version )
#define mpi_get_processor_name FTN_NAME ( MPI_GET_PROCESSOR_NAME , mpi_get_processor_name )
#define mpi_errhandler_create FTN_NAME( MPI_ERRHANDLER_CREATE , mpi_errhandler_create )
#define mpi_errhandler_set FTN_NAME( MPI_ERRHANDLER_SET , mpi_errhandler_set )
#define mpi_errhandler_get FTN_NAME( MPI_ERRHANDLER_GET , mpi_errhandler_get )
#define mpi_errhandler_free FTN_NAME( MPI_ERRHANDLER_FREE , mpi_errhandler_free )
#define mpi_add_error_code FTN_NAME( MPI_ADD_ERROR_CODE , mpi_add_error_code )
#define mpi_add_error_class FTN_NAME( MPI_ADD_ERROR_CLASS , mpi_add_error_class )
/* mpi_add_error_string is defined in ampifimpl.f90, see ampif_add_error_string defined below */
#define mpi_error_class FTN_NAME( MPI_ERROR_CLASS , mpi_error_class )
#define mpi_error_string FTN_NAME( MPI_ERROR_STRING , mpi_error_string )
#define mpi_wtime FTN_NAME( MPI_WTIME , mpi_wtime )
#define mpi_wtick FTN_NAME( MPI_WTICK , mpi_wtick )
#define mpi_is_thread_main FTN_NAME( MPI_IS_THREAD_MAIN , mpi_is_thread_main )
#define mpi_query_thread FTN_NAME( MPI_QUERY_THREAD , mpi_query_thread )
#define mpi_init_thread FTN_NAME( MPI_INIT_THREAD , mpi_init_thread )
#define mpi_init FTN_NAME( MPI_INIT , mpi_init )
#define mpi_initialized FTN_NAME( MPI_INITIALIZED , mpi_initialized )
#define mpi_finalize FTN_NAME( MPI_FINALIZE , mpi_finalize )
#define mpi_finalized FTN_NAME( MPI_FINALIZED , mpi_finalized )
#define mpi_abort FTN_NAME( MPI_ABORT , mpi_abort )
#define mpi_file_call_errhandler FTN_NAME( MPI_FILE_CALL_ERRHANDLER , mpi_file_call_errhandler )
#define mpi_file_create_errhandler FTN_NAME( MPI_FILE_CREATE_ERRHANDLER , mpi_file_create_errhandler )
#define mpi_file_set_errhandler FTN_NAME( MPI_FILE_SET_ERRHANDLER , mpi_file_set_errhandler )
#define mpi_file_get_errhandler FTN_NAME( MPI_FILE_GET_ERRHANDLER , mpi_file_get_errhandler )

/* MPI-2 */
#define mpi_type_get_envelope FTN_NAME ( MPI_TYPE_GET_ENVELOPE , mpi_type_get_envelope )
#define mpi_type_get_contents FTN_NAME ( MPI_TYPE_GET_CONTENTS , mpi_type_get_contents )

#define mpi_win_create FTN_NAME ( MPI_WIN_CREATE , mpi_win_create )
#define mpi_win_allocate FTN_NAME ( MPI_WIN_ALLOCATE , mpi_win_allocate )
#define mpi_win_free  FTN_NAME ( MPI_WIN_FREE  , mpi_win_free )
#define mpi_win_create_errhandler FTN_NAME ( MPI_WIN_CREATE_ERRHANDLER , mpi_win_create_errhandler )
#define mpi_win_call_errhandler FTN_NAME ( MPI_WIN_CALL_ERRHANDLER , mpi_win_call_errhandler )
#define mpi_win_get_errhandler FTN_NAME ( MPI_WIN_GET_ERRHANDLER , mpi_win_get_errhandler )
#define mpi_win_set_errhandler FTN_NAME ( MPI_WIN_SET_ERRHANDLER , mpi_win_set_errhandler )
#define mpi_win_create_keyval FTN_NAME ( MPI_WIN_CREATE_KEYVAL , mpi_win_create_keyval )
#define mpi_win_free_keyval FTN_NAME ( MPI_WIN_FREE_KEYVAL , mpi_win_free_keyval )
#define mpi_win_delete_attr  FTN_NAME ( MPI_WIN_DELETE_ATTR  , mpi_win_delete_attr )
#define mpi_win_get_attr  FTN_NAME ( MPI_WIN_GET_ATTR  , mpi_win_get_attr )
#define mpi_win_set_attr  FTN_NAME ( MPI_WIN_SET_ATTR  , mpi_win_set_attr )
#define mpi_win_get_group  FTN_NAME ( MPI_WIN_GET_GROUP  , mpi_win_get_group )
/* mpi_win_set_name is defined in ampifimpl.f90, see ampif_win_set_name defined below */
#define mpi_win_get_name  FTN_NAME ( MPI_WIN_GET_NAME  , mpi_win_get_name )
#define mpi_win_set_info  FTN_NAME ( MPI_WIN_SET_INFO  , mpi_win_set_info )
#define mpi_win_get_info  FTN_NAME ( MPI_WIN_GET_INFO  , mpi_win_get_info )
#define mpi_win_fence  FTN_NAME ( MPI_WIN_FENCE  , mpi_win_fence )
#define mpi_win_lock  FTN_NAME ( MPI_WIN_LOCK  , mpi_win_lock )
#define mpi_win_lock_all  FTN_NAME ( MPI_WIN_LOCK_ALL , mpi_win_lock_all )
#define mpi_win_unlock  FTN_NAME ( MPI_WIN_UNLOCK  , mpi_win_unlock )
#define mpi_win_unlock_all  FTN_NAME ( MPI_WIN_UNLOCK_ALL  , mpi_win_unlock_all )
#define mpi_win_post  FTN_NAME ( MPI_WIN_POST  , mpi_win_post )
#define mpi_win_wait  FTN_NAME ( MPI_WIN_WAIT  , mpi_win_wait )
#define mpi_win_start  FTN_NAME ( MPI_WIN_START  , mpi_win_start )
#define mpi_win_complete  FTN_NAME ( MPI_WIN_COMPLETE  , mpi_win_complete )
#define mpi_win_test FTN_NAME ( MPI_WIN_TEST , mpi_win_test )
#define mpi_alloc_mem  FTN_NAME ( MPI_ALLOC_MEM  , mpi_alloc_mem )
#define mpi_free_mem  FTN_NAME ( MPI_FREE_MEM  , mpi_free_mem )
#define mpi_put  FTN_NAME ( MPI_PUT  , mpi_put )
#define mpi_get  FTN_NAME ( MPI_GET  , mpi_get )
#define mpi_accumulate  FTN_NAME ( MPI_ACCUMULATE  , mpi_accumulate )
#define mpi_get_accumulate  FTN_NAME ( MPI_GET_ACCUMULATE  , mpi_get_accumulate )
#define mpi_rput  FTN_NAME ( MPI_RPUT  , mpi_rput )
#define mpi_rget  FTN_NAME ( MPI_RGET  , mpi_rget )
#define mpi_raccumulate  FTN_NAME ( MPI_RACCUMULATE  , mpi_raccumulate )
#define mpi_rget_accumulate  FTN_NAME ( MPI_RGET_ACCUMULATE  , mpi_rget_accumulate )
#define mpi_fetch_and_op  FTN_NAME ( MPI_FETCH_AND_OP  , mpi_fetch_and_op )
#define mpi_compare_and_swap  FTN_NAME ( MPI_COMPARE_AND_SWAP  , mpi_compare_and_swap )

#define mpi_info_create FTN_NAME ( MPI_INFO_CREATE , mpi_info_create )
/* mpi_info_set is defined in ampifimpl.f90, see ampif_info_set defined below */
/* mpi_info_delete is defined in ampifimpl.f90, see ampif_info_delete defined below */
/* mpi_info_get is defined in ampifimpl.f90, see ampif_info_get defined below */
/* mpi_info_get_valuelen is defined in ampifimpl.f90, see ampif_info_get_valuelen defined below */
#define mpi_info_get_nkeys FTN_NAME ( MPI_INFO_GET_NKEYS , mpi_info_get_nkeys )
#define mpi_info_get_nthkey FTN_NAME ( MPI_INFO_GET_NTHKEYS , mpi_info_get_nthkey )
#define mpi_info_dup FTN_NAME ( MPI_INFO_DUP , mpi_info_dup )
#define mpi_info_free FTN_NAME ( MPI_INFO_FREE , mpi_info_free )

#define mpi_pcontrol FTN_NAME ( MPI_PCONTROL , mpi_pcontrol )

/* Functions that take 'const char*' arguments are wrapped by Fortran subroutines
 * defined in ampifimpl.f90. These functions should be prefixed with 'ampif_' here: */
#define ampif_comm_set_name FTN_NAME ( AMPIF_COMM_SET_NAME , ampif_comm_set_name )
#define ampif_type_set_name FTN_NAME ( AMPIF_TYPE_SET_NAME , ampif_type_set_name )
#define ampif_win_set_name FTN_NAME ( AMPIF_WIN_SET_NAME , ampif_win_set_name )
#define ampif_info_set FTN_NAME ( AMPIF_INFO_SET , ampif_info_set )
#define ampif_info_delete FTN_NAME ( AMPIF_INFO_DELETE , ampif_info_delete )
#define ampif_info_get FTN_NAME ( AMPIF_INFO_GET , ampif_info_get )
#define ampif_info_get_valuelen FTN_NAME ( AMPIF_INFO_GET_VALUELEN , ampif_info_get_valuelen )
#define ampif_add_error_string FTN_NAME ( AMPIF_ADD_ERROR_STRING , ampif_add_error_string )
#define ampif_print FTN_NAME( AMPIF_PRINT , ampif_print )

/* Extensions needed by ROMIO */
#define mpir_status_set_bytes FTN_NAME ( MPIR_STATUS_SET_BYTES, mpir_status_set_bytes )

/* AMPI extensions */
#define ampi_migrate FTN_NAME( AMPI_MIGRATE , ampi_migrate )
#define ampi_load_start_measure FTN_NAME( AMPI_LOAD_START_MEASURE, ampi_load_start_measure )
#define ampi_load_stop_measure FTN_NAME( AMPI_LOAD_STOP_MEASURE, ampi_load_stop_measure )
#define ampi_load_reset_measure FTN_NAME( AMPI_LOAD_RESET_MEASURE, ampi_load_reset_measure )
#define ampi_load_set_value FTN_NAME( AMPI_LOAD_SET_VALUE, ampi_load_set_value )
#define ampi_load_set_phase FTN_NAME( AMPI_LOAD_SET_PHASE, ampi_load_set_phase )
#define ampi_migrate_to_pe FTN_NAME( AMPI_MIGRATE_TO_PE , ampi_migrate_to_pe )
#define ampi_set_migratable FTN_NAME ( AMPI_SET_MIGRATABLE , ampi_set_migratable )
#define ampi_register_pup FTN_NAME( AMPI_REGISTER_PUP , ampi_register_pup )
#define ampi_register_about_to_migrate FTN_NAME ( AMPI_REGISTER_ABOUT_TO_MIGRATE , ampi_register_about_to_migrate )
#define ampi_register_just_migrated FTN_NAME ( AMPI_REGISTER_JUST_MIGRATED , ampi_register_just_migrated )
#define ampi_type_is_contiguous FTN_NAME ( AMPI_TYPE_IS_CONTIGUOUS , ampi_type_is_contiguous )
#define ampi_get_pup_data FTN_NAME ( AMPI_GET_PUP_DATA , ampi_get_pup_data )
#define ampi_iget FTN_NAME ( AMPI_IGET  , ampi_iget )
#define ampi_iget_wait FTN_NAME ( AMPI_IGET_WAIT  , ampi_iget_wait )
#define ampi_iget_data FTN_NAME ( AMPI_IGET_DATA  , ampi_iget_data )
#define ampi_iget_free FTN_NAME ( AMPI_IGET_FREE  , ampi_iget_free )
#define ampi_alltoall_medium FTN_NAME( AMPI_ALLTOALL_MEDIUM , ampi_alltoall_medium )
#define ampi_alltoall_long FTN_NAME( AMPI_ALLTOALL_LONG , ampi_alltoall_long )
#define ampi_yield FTN_NAME ( AMPI_YIELD , ampi_yield )
#define ampi_suspend FTN_NAME ( AMPI_SUSPEND , ampi_suspend )
#define ampi_resume FTN_NAME ( AMPI_RESUME, ampi_resume )
/* ampi_print is defined in ampifimpl.f90, see ampif_print defined below */
#define ampi_install_idle_timer FTN_NAME( AMPI_INSTALL_IDLE_TIMER , ampi_install_idle_timer )
#define ampi_uninstall_idle_timer FTN_NAME( AMPI_UNINSTALL_IDLE_TIMER , ampi_uninstall_idle_timer )
#define ampi_trace_begin FTN_NAME( AMPI_TRACE_BEGIN , ampi_trace_begin )
#define ampi_trace_end FTN_NAME( AMPI_TRACE_END , ampi_trace_end )

/* Fortran-specific AMPI extensions */
#define ampi_command_argument_count FTN_NAME( AMPI_COMMAND_ARGUMENT_COUNT , ampi_command_argument_count )
#define ampi_get_command_argument FTN_NAME( AMPI_GET_COMMAND_ARGUMENT , ampi_get_command_argument )


#if CMK_CUDA
#define ampi_gpu_invoke_wr FTN_NAME ( AMPI_GPU_INVOKE_WR  , ampi_gpu_invoke_wr )
#define ampi_gpu_iinvoke_wr FTN_NAME ( AMPI_GPU_IINVOKE_WR  , ampi_gpu_iinvoke_wr )
#endif

#define REDUCERF(caps, nocaps) \
void FTN_NAME(caps, nocaps)(void *iv, void *iov, int *len, MPI_Datatype *dt){ \
  caps(iv, iov, len, dt); \
}

/* Strings passed from Fortran must be explicitly NULL terminated
 * before they are passed into AMPI. */
static void ampif_str_f2c(char* dst, const char* src, int len) noexcept {
  memcpy(dst, src, len);
  dst[len] = '\0';
}

/* Strings passed to Fortran must be backfilled with blank spaces. */
static void ampif_str_c2f(char* dst, const char* src, int max_len) noexcept {
  strncpy(dst, src, max_len);
  for(int i=strlen(src); i<max_len; i++)
    dst[i] = ' ';
}

static MPI_Status* handle_MPI_STATUS_IGNORE(int *sts) noexcept {
  if (sts[0]==-9 && sts[1]==-9 && sts[2]==-9 && sts[3]==-9 &&
      sts[4]==-9 && sts[5]==-9 && sts[6]==-9 && sts[7]==-9) {
    return MPI_STATUS_IGNORE;
  } else {
    return (MPI_Status *)sts;
  }
}

static MPI_Status* handle_MPI_STATUSES_IGNORE(int *sts) noexcept {
  if (sts[0]==-9 && sts[1]==-9 && sts[2]==-9 && sts[3]==-9 &&
      sts[4]==-9 && sts[5]==-9 && sts[6]==-9 && sts[7]==-9) {
    return MPI_STATUSES_IGNORE;
  } else {
    return (MPI_Status *)sts;
  }
}

static void handle_MPI_IN_PLACE_f(void *& inbuf, void *& outbuf) noexcept {
  if (inbuf == NULL) inbuf = MPI_IN_PLACE;
  if (outbuf == NULL) outbuf = MPI_IN_PLACE;
}

void mpi_is_thread_main(int *flag, int *ierr) noexcept
{
  *ierr = MPI_Is_thread_main(flag);
}

void mpi_query_thread(int *provided, int *ierr) noexcept
{
  *ierr = MPI_Query_thread(provided);
}

void mpi_init_thread(int *required, int *provided, int *ierr) noexcept
{
  *ierr = MPI_Init_thread(NULL, NULL, *required, provided);
}

void mpi_init(int *ierr) noexcept
{
  *ierr = MPI_Init(NULL,NULL);
}

void mpi_initialized(int *isInit, int* ierr) noexcept
{
  *ierr = MPI_Initialized(isInit);
}

void mpi_finalized(int *isFinalized, int* ierr) noexcept
{
  *ierr = MPI_Finalized(isFinalized);
}

void mpi_comm_rank(int *comm, int *rank, int *ierr) noexcept
{
  *ierr = MPI_Comm_rank(*comm, rank);
}

void mpi_comm_size(int *comm, int *size, int *ierr) noexcept
{
  *ierr = MPI_Comm_size(*comm, size);
}

void mpi_finalize(int *ierr) noexcept
{
  *ierr = MPI_Finalize();
}

void mpi_send(void *msg, int *count, int *type, int *dest,
              int *tag, int *comm, int *ierr) noexcept
{
  *ierr = MPI_Send(msg, *count, *type, *dest, *tag, *comm);
}

void mpi_recv(void *msg, int *count, int *type, int *src,
              int *tag, int *comm, int *status, int *ierr) noexcept
{
  MPI_Status* s = handle_MPI_STATUS_IGNORE(status);
  *ierr = MPI_Recv(msg, *count, *type, *src, *tag, *comm, s);
}

void mpi_bsend(void *msg, int *count, int *type, int *dest,
               int *tag, int *comm, int *ierr) noexcept
{
  *ierr = MPI_Bsend(msg, *count, *type, *dest, *tag, *comm);
}

void mpi_buffer_attach(void *buffer, int *size, int *ierr) noexcept
{
  *ierr = MPI_Buffer_attach(buffer, *size);
}

void mpi_buffer_detach(void *buffer, int *size, int *ierr) noexcept
{
  *ierr = MPI_Buffer_detach(buffer, size);
}

void mpi_rsend(void *msg, int *count, int *type, int *dest,
               int *tag, int *comm, int *ierr) noexcept
{
  *ierr = MPI_Rsend(msg, *count, *type, *dest, *tag, *comm);
}

void mpi_ssend(void *msg, int *count, int *type, int *dest,
               int *tag, int *comm, int *ierr) noexcept
{
  *ierr = MPI_Ssend(msg, *count, *type, *dest, *tag, *comm);
}

void mpi_issend(void *buf, int *count, int *datatype, int *dest,
                int *tag, int *comm, int *request, int *ierr) noexcept
{
  *ierr = MPI_Issend(buf, *count, *datatype, *dest, *tag, *comm, (MPI_Request *)request);
}

void mpi_probe(int *src, int *tag, int *comm, int *status, int *ierr) noexcept
{
  MPI_Status* s = handle_MPI_STATUS_IGNORE(status);
  *ierr = MPI_Probe(*src, *tag, *comm, s);
}

void mpi_iprobe(int *src, int *tag, int *comm, int *flag, int *status, int *ierr) noexcept
{
  MPI_Status* s = handle_MPI_STATUS_IGNORE(status);
  *ierr = MPI_Iprobe(*src, *tag, *comm, flag, s);
}

void mpi_mprobe(int *src, int *tag, int *comm, int *message, int *status, int *ierr) noexcept
{
  MPI_Status* s = handle_MPI_STATUS_IGNORE(status);
  *ierr = MPI_Mprobe(*src, *tag, *comm, message, s);
}

void mpi_improbe(int *src, int *tag, int *comm, int *flag, int *message, int* status, int *ierr) noexcept
{
  MPI_Status* s = handle_MPI_STATUS_IGNORE(status);
  *ierr = MPI_Improbe(*src, *tag, *comm, flag, message, s);
}

void mpi_sendrecv(void *sndbuf, int *sndcount, int *sndtype,
                  int *dest, int *sndtag, void *rcvbuf,
                  int *rcvcount, int *rcvtype, int *src,
                  int *rcvtag, int *comm, int *status, int *ierr) noexcept
{
  MPI_Status* s = handle_MPI_STATUS_IGNORE(status);
  *ierr = MPI_Sendrecv(sndbuf, *sndcount, *sndtype, *dest, *sndtag,
                       rcvbuf, *rcvcount, *rcvtype, *src, *rcvtag,
                       *comm, s);
}

void mpi_sendrecv_replace(void *buf, int* count, int* datatype,
                          int* dest, int* sendtag, int* source, int* recvtag,
                          int* comm, int* status, int *ierr) noexcept
{
  MPI_Status* s = handle_MPI_STATUS_IGNORE(status);
  *ierr = MPI_Sendrecv_replace(buf, *count, *datatype, *dest, *sendtag,
                               *source, *recvtag, *comm, s);
}

void mpi_type_match_size(int *typeclass, int *size, int *datatype, int *ierr)
{
    *ierr = MPI_Type_match_size(*typeclass, *size, (MPI_Datatype*)datatype);
}

void mpi_barrier(int *comm, int *ierr) noexcept
{
  *ierr = MPI_Barrier(*comm);
}

void mpi_ibarrier(int *comm, int *request, int *ierr) noexcept
{
  *ierr = MPI_Ibarrier(*comm, request);
}

void mpi_bcast(void *buf, int *count, int *type, int *root, int *comm, int *ierr) noexcept
{
  *ierr = MPI_Bcast(buf, *count, *type, *root, *comm);
}

void mpi_ibcast(void *buf, int *count, int *type, int *root, int *comm,
                int *request, int *ierr) noexcept
{
  *ierr = MPI_Ibcast(buf, *count, *type, *root, *comm, request);
}

void mpi_reduce(void *inbuf, void *outbuf, int *count, int *type,
                int *op, int *root, int *comm, int *ierr) noexcept
{
  handle_MPI_IN_PLACE_f(inbuf, outbuf);
  *ierr = MPI_Reduce(inbuf, outbuf, *count, *type, *op, *root, *comm);
}

void mpi_allreduce(void *inbuf,void *outbuf,int *count,int *type,
                   int *op, int *comm, int *ierr) noexcept
{
  handle_MPI_IN_PLACE_f(inbuf, outbuf);
  *ierr = MPI_Allreduce(inbuf, outbuf, *count, *type, *op, *comm);
}

double mpi_wtime() noexcept
{
  return MPI_Wtime();
}

double mpi_wtick() noexcept
{
  return MPI_Wtick();
}

void mpi_start(int *reqnum, int *ierr) noexcept
{
  *ierr = MPI_Start((MPI_Request*) reqnum);
}

void mpi_startall(int *count, int *reqnum, int *ierr) noexcept
{
  *ierr = MPI_Startall(*count, (MPI_Request*) reqnum);
}

void mpi_waitall(int *count, int *request, int *status, int *ierr) noexcept
{
  MPI_Status* s = handle_MPI_STATUSES_IGNORE(status);
  *ierr = MPI_Waitall(*count, (MPI_Request*) request, s);
}

void mpi_waitany(int *count, int *request, int *index, int *status, int *ierr) noexcept
{
  MPI_Status* s = handle_MPI_STATUS_IGNORE(status);
  *ierr = MPI_Waitany(*count, (MPI_Request*) request, index, s);
  if (*index != MPI_UNDEFINED)
    (*index)++;
}

void mpi_wait(int *request, int *status, int *ierr) noexcept
{
  MPI_Status* s = handle_MPI_STATUS_IGNORE(status);
  *ierr = MPI_Wait((MPI_Request*) request, s);
}

void mpi_testall(int *count, int *request, int *flag, int *status, int *ierr) noexcept
{
  MPI_Status* s = handle_MPI_STATUSES_IGNORE(status);
  *ierr = MPI_Testall(*count, (MPI_Request*) request, flag, s);
}

void mpi_waitsome(int *incount, int *array_of_requests, int *outcount, int *array_of_indices,
                  int *array_of_statuses, int *ierr) noexcept
{
  MPI_Status* s = handle_MPI_STATUSES_IGNORE(array_of_statuses);
  *ierr = MPI_Waitsome(*incount, (MPI_Request *)array_of_requests, outcount, array_of_indices, s);
  if (*outcount != MPI_UNDEFINED) {
    for (int i = 0; i < *outcount; ++i) {
      array_of_indices[i]++;
    }
  }
}

void mpi_testsome(int *incount, int *array_of_requests, int *outcount, int *array_of_indices,
                  int *array_of_statuses, int *ierr) noexcept
{
  MPI_Status* s = handle_MPI_STATUSES_IGNORE(array_of_statuses);
  *ierr = MPI_Testsome(*incount, (MPI_Request *)array_of_requests, outcount, array_of_indices, s);
  if (*outcount != MPI_UNDEFINED) {
    for (int i = 0; i < *outcount; ++i) {
      array_of_indices[i]++;
    }
  }
}

void mpi_testany(int *count, int *request, int *index, int *flag, int *status, int *ierr) noexcept
{
  MPI_Status* s = handle_MPI_STATUS_IGNORE(status);
  *ierr = MPI_Testany(*count, (MPI_Request*) request, index, flag, s);
  if (*index != MPI_UNDEFINED)
    (*index)++;
}

void mpi_test(int *request, int *flag, int *status, int *ierr) noexcept
{
  MPI_Status* s = handle_MPI_STATUS_IGNORE(status);
  *ierr = MPI_Test((MPI_Request*) request, flag, s);
}

void mpi_request_get_status(int *request, int *flag, int *status, int *ierr) noexcept
{
  MPI_Status* s = handle_MPI_STATUS_IGNORE(status);
  *ierr = MPI_Request_get_status(*((MPI_Request*)request), flag, s);
}

void mpi_request_free(int *request, int *ierr) noexcept
{
  *ierr = MPI_Request_free((MPI_Request *)request);
}

void mpi_grequest_start(int (*query_fn)(void*,MPI_Status*), int (*free_fn)(void*),
                        int (*cancel_fn)(void*,int), void *extra_state, int *request, int *ierr) noexcept
{
  *ierr = MPI_Grequest_start(query_fn, free_fn, cancel_fn, extra_state, request);
}

void mpi_grequest_complete(int *request, int *ierr) noexcept
{
  *ierr = MPI_Grequest_complete(*request);
}

void mpi_cancel(int *request, int *ierr) noexcept
{
  *ierr = MPI_Cancel((MPI_Request *)request);
}

void mpi_test_cancelled(int *status, int *flag, int *ierr) noexcept
{
  MPI_Status* s = handle_MPI_STATUS_IGNORE(status);
  *ierr = MPI_Test_cancelled(s, flag);
}

void mpi_status_set_cancelled(int *status, int *flag, int *ierr) noexcept
{
  MPI_Status* s = handle_MPI_STATUS_IGNORE(status);
  *ierr = MPI_Status_set_cancelled(s, *flag);
}

void mpi_recv_init(void *buf, int *count, int *type, int *srcpe,
                   int *tag, int *comm, int *req, int *ierr) noexcept
{
  *ierr = MPI_Recv_init(buf,*count,*type,*srcpe,*tag,*comm,(MPI_Request*)req);
}

void mpi_send_init(void *buf, int *count, int *type, int *destpe,
                   int *tag, int *comm, int *req, int *ierr) noexcept
{
  *ierr = MPI_Send_init(buf,*count,*type,*destpe,*tag,*comm,(MPI_Request*)req);
}

void mpi_rsend_init(void *buf, int *count, int *type, int *destpe,
                   int *tag, int *comm, int *req, int *ierr) noexcept
{
  *ierr = MPI_Rsend_init(buf,*count,*type,*destpe,*tag,*comm,(MPI_Request*)req);
}

void mpi_bsend_init(void *buf, int *count, int *type, int *destpe,
                   int *tag, int *comm, int *req, int *ierr) noexcept
{
  *ierr = MPI_Bsend_init(buf,*count,*type,*destpe,*tag,*comm,(MPI_Request*)req);
}

void mpi_type_contiguous(int *count, int *oldtype, int *newtype, int *ierr) noexcept
{
  *ierr = MPI_Type_contiguous(*count, *oldtype, newtype);
}

void mpi_type_vector(int *count, int *blocklength, int *stride,
                     int *oldtype, int*  newtype, int *ierr) noexcept
{
  *ierr = MPI_Type_vector(*count, *blocklength, *stride, *oldtype, newtype);
}

void mpi_type_create_hvector(int *count, int *blocklength, int *stride,
                             int* oldtype, int* newtype, int* ierr) noexcept
{
  *ierr = MPI_Type_create_hvector(*count, *blocklength, *stride, *oldtype, newtype);
}

void mpi_type_hvector(int *count, int *blocklength, int *stride,
                      int* oldtype, int* newtype, int* ierr) noexcept
{
  *ierr = MPI_Type_hvector(*count, *blocklength, *stride, *oldtype, newtype);
}

void mpi_type_indexed(int *count, int* arrBlength, int* arrDisp,
                      int* oldtype, int*  newtype, int* ierr) noexcept
{
  *ierr = MPI_Type_indexed(*count, arrBlength, arrDisp, *oldtype, newtype);
}

void mpi_type_create_hindexed(int* count, int* arrBlength, MPI_Aint* arrDisp,
                              int* oldtype, int* newtype, int* ierr) noexcept
{
  *ierr = MPI_Type_create_hindexed(*count, arrBlength, arrDisp, *oldtype, newtype);
}

void mpi_type_hindexed(int* count, int* arrBlength, MPI_Aint* arrDisp,
                       int* oldtype, int* newtype, int* ierr) noexcept
{
  *ierr = MPI_Type_hindexed(*count, arrBlength, arrDisp, *oldtype, newtype);
}

void mpi_type_create_indexed_block(int* count, int* Blength, int* arr,
                                   int* oldtype, int* newtype, int* ierr) noexcept
{
  *ierr = MPI_Type_create_indexed_block(*count, *Blength, arr, *oldtype, newtype);
}

void mpi_type_create_hindexed_block(int* count, int* Blength, MPI_Aint* arr,
                                    int* oldtype, int* newtype, int* ierr) noexcept
{
  *ierr = MPI_Type_create_hindexed_block(*count, *Blength, arr, *oldtype, newtype);
}

void mpi_type_create_struct(int* count, int* arrBlength, MPI_Aint* arrDisp,
                            int* oldtype, int* newtype, int* ierr) noexcept
{
  *ierr = MPI_Type_create_struct(*count, arrBlength, arrDisp, oldtype, newtype);
}

void mpi_type_struct(int* count, int* arrBlength, MPI_Aint* arrDisp,
                     int* oldtype, int* newtype, int* ierr) noexcept
{
  *ierr = MPI_Type_struct(*count, arrBlength, arrDisp, oldtype, newtype);
}

void mpi_type_create_resized(int* oldtype, MPI_Aint* lb, MPI_Aint* extent, int* newtype, int* ierr) noexcept
{
  *ierr = MPI_Type_create_resized(*oldtype, *lb, *extent, newtype);
}

void mpi_type_dup(int* oldtype, int* newtype, int* ierr) noexcept
{
  *ierr = MPI_Type_dup(*oldtype, newtype);
}

void mpi_type_commit(int *type, int *ierr) noexcept
{
  *ierr = MPI_Type_commit(type);
}

void mpi_type_free(int *type, int *ierr) noexcept
{
  *ierr = MPI_Type_free(type);
}

void mpi_type_create_darray(int *size, int *rank, int *ndims, int *array_of_gsizes,
  int *array_of_distribs, int *array_of_dargs, int *array_of_psizes, int *order,
  int *oldtype, int *newtype, int *ierr) noexcept
{
  *ierr = MPI_Type_create_darray(*size, *rank, *ndims, array_of_gsizes,
    array_of_distribs, array_of_dargs, array_of_psizes, *order,
    (MPI_Datatype)*oldtype, (MPI_Datatype*)newtype);
}

void mpi_type_create_subarray(int *ndims, int *array_of_sizes, int *array_of_subsizes,
  int *array_of_starts, int *order, int *oldtype, int *newtype, int *ierr) noexcept
{
  *ierr = MPI_Type_create_subarray(*ndims, array_of_sizes, array_of_subsizes,
    array_of_starts, *order, (MPI_Datatype)*oldtype, (MPI_Datatype*)newtype);
}


void  mpi_type_get_extent(int* type, MPI_Aint* lb, MPI_Aint* extent, int* ierr) noexcept
{
  *ierr = MPI_Type_get_extent(*type, lb, extent);
}

void  mpi_type_get_extent_x(int* type, MPI_Count* lb, MPI_Count* extent, int* ierr) noexcept
{
  *ierr = MPI_Type_get_extent_x(*type, lb, extent);
}

void  mpi_type_extent(int* type, MPI_Aint* extent, int* ierr) noexcept
{
  *ierr = MPI_Type_extent(*type, extent);
}

void mpi_type_get_true_extent(int* datatype, MPI_Aint* true_lb, MPI_Aint* true_extent, int* ierr) noexcept
{
  *ierr = MPI_Type_get_true_extent(*datatype, true_lb, true_extent);
}

void mpi_type_get_true_extent_x(int* datatype, MPI_Count* true_lb, MPI_Count* true_extent, int* ierr) noexcept
{
  *ierr = MPI_Type_get_true_extent_x(*datatype, true_lb, true_extent);
}

void  mpi_type_size(int* type, int* size, int* ierr) noexcept
{
  *ierr = MPI_Type_size(*type, size);
}

void  mpi_type_size_x(int* type, MPI_Count* size, int* ierr) noexcept
{
  *ierr = MPI_Type_size_x(*type, size);
}

void mpi_type_lb(int* datatype, MPI_Aint* displacement, int* ierr) noexcept
{
  *ierr = MPI_Type_lb(*datatype, displacement);
}

void mpi_type_ub(int* datatype, MPI_Aint* displacement, int* ierr) noexcept
{
  *ierr = MPI_Type_ub(*datatype, displacement);
}

void ampif_type_set_name(int* datatype, const char* name, int *nlen, int* ierr) noexcept
{
  char tmpName[MPI_MAX_OBJECT_NAME];
  ampif_str_f2c(tmpName, name, *nlen);

  *ierr = MPI_Type_set_name(*datatype, tmpName);
}

void mpi_type_get_name(int* datatype, char* name, int* resultlen, int* ierr) noexcept
{
  char tmpName[MPI_MAX_OBJECT_NAME];

  *ierr = MPI_Type_get_name(*datatype, tmpName, resultlen);

  if (*ierr == MPI_SUCCESS)
    ampif_str_c2f(name, tmpName, MPI_MAX_OBJECT_NAME);
}

void mpi_type_set_attr(int *datatype, int *type_keyval, void *attribute_val, int *ierr) noexcept
{
  *ierr = MPI_Type_set_attr(*datatype, *type_keyval, attribute_val);
}

void mpi_type_get_attr(int *datatype, int *type_keyval, void *attribute_val, int *flag, int *ierr) noexcept
{
  int * result;
  *ierr = MPI_Type_get_attr(*datatype, *type_keyval, &result, flag);
  *(intptr_t *)attribute_val = *result;
}

void mpi_type_delete_attr(int *datatype, int *type_keyval, int *ierr) noexcept
{
  *ierr = MPI_Type_delete_attr(*datatype, *type_keyval);
}

void mpi_type_create_keyval(int (*type_copy_attr_fn)(int,int,void*,void*,void*,int*),
                            int (*type_delete_attr_function)(int,int,void*,void*),
                            int *type_keyval, void *extra_state, int *ierr) noexcept
{
  *ierr = MPI_Type_create_keyval(type_copy_attr_fn, type_delete_attr_function,
                                 type_keyval, extra_state);
}

void mpi_type_free_keyval(int *type_keyval, int *ierr) noexcept
{
  *ierr = MPI_Type_free_keyval(type_keyval);
}

void mpi_get_address(const void* location, MPI_Aint *address, int* ierr) noexcept
{
  *ierr = MPI_Get_address(location, address);
}

void mpi_address(void* location, MPI_Aint *address, int* ierr) noexcept
{
  *ierr = MPI_Address(location, address);
}

void mpi_status_set_elements(int *status, int* datatype, int *count, int* ierr) noexcept
{
  MPI_Status* s = handle_MPI_STATUS_IGNORE(status);
  *ierr = MPI_Status_set_elements(s, *datatype, *count);
}

void mpi_status_set_elements_x(int *status, int* datatype, MPI_Count *count, int* ierr) noexcept
{
  MPI_Status* s = handle_MPI_STATUS_IGNORE(status);
  *ierr = MPI_Status_set_elements_x(s, *datatype, *count);
}

void mpi_get_elements(int *status, int* datatype, int *count, int* ierr) noexcept
{
  MPI_Status* s = handle_MPI_STATUS_IGNORE(status);
  *ierr = MPI_Get_elements(s, *datatype, count);
}

void mpi_get_elements_x(int *status, int* datatype, MPI_Count *count, int* ierr) noexcept
{
  MPI_Status* s = handle_MPI_STATUS_IGNORE(status);
  *ierr = MPI_Get_elements_x(s, *datatype, count);
}

void mpi_pack(void *inbuf, int *incount, int *datatype, void *outbuf,
              int *outsize, int *position, int *comm, int *ierr) noexcept
{
  *ierr = MPI_Pack(inbuf, *incount, (MPI_Datatype)*datatype, outbuf,
                   *outsize, position, *comm);
}

void mpi_unpack(void *inbuf, int *insize, int *position, void *outbuf,
                int *outcount, int *datatype, int *comm, int *ierr) noexcept
{
  *ierr = MPI_Unpack(inbuf, *insize, position, outbuf, *outcount,
                     (MPI_Datatype) *datatype, (MPI_Comm) *comm);
}

void mpi_pack_size(int *incount, int *datatype, int *comm, int *size, int *ierr) noexcept
{
  *ierr = MPI_Pack_size(*incount, (MPI_Datatype) *datatype, *comm, size);
}

MPI_Aint mpi_aint_add(MPI_Aint *addr, MPI_Aint *disp) noexcept
{
  return MPI_Aint_add(*addr, *disp);
}

MPI_Aint mpi_aint_diff(MPI_Aint *addr1, MPI_Aint *addr2) noexcept
{
  return MPI_Aint_diff(*addr1, *addr2);
}

void mpi_isend(void *buf, int *count, int *datatype, int *dest,
               int *tag, int *comm, int *request, int *ierr) noexcept
{
  *ierr = MPI_Isend(buf, *count, *datatype, *dest, *tag, *comm, (MPI_Request *)request);
}

void mpi_ibsend(void *buf, int *count, int *datatype, int *dest,
                int *tag, int *comm, int *request, int *ierr) noexcept
{
  *ierr = MPI_Ibsend(buf, *count, *datatype, *dest, *tag, *comm, (MPI_Request *)request);
}

void mpi_irsend(void *buf, int *count, int *datatype, int *dest,
                int *tag, int *comm, int *request, int *ierr) noexcept
{
  *ierr = MPI_Irsend(buf, *count, *datatype, *dest, *tag, *comm, (MPI_Request *)request);
}

void mpi_irecv(void *buf, int *count, int *datatype, int *src,
               int *tag, int *comm, int *request, int *ierr) noexcept
{
  *ierr = MPI_Irecv(buf, *count, *datatype, *src, *tag, *comm, (MPI_Request *)request);
}

void mpi_mrecv(void *buf, int *count, int *datatype, int *message,
               int *status, int *ierr) noexcept
{
  *ierr = MPI_Mrecv(buf, *count, *datatype, message, (MPI_Status*) status);
}

void mpi_imrecv(void *buf, int *count, int *datatype, int *message,
                int *request, int *ierr) noexcept
{
  *ierr = MPI_Imrecv(buf, *count, *datatype, message, (MPI_Request*) request);
}

void mpi_allgatherv(void *sendbuf, int *sendcount, int *sendtype,
                    void *recvbuf, int *recvcounts, int *displs,
                    int *recvtype, int *comm, int *ierr) noexcept
{
  handle_MPI_IN_PLACE_f(sendbuf, recvbuf);
  *ierr = MPI_Allgatherv(sendbuf, *sendcount, *sendtype, recvbuf, recvcounts,
                         displs, *recvtype, *comm);
}

void mpi_iallgatherv(void *sendbuf, int *sendcount, int *sendtype,
                     void *recvbuf, int *recvcounts, int *displs,
                     int *recvtype, int *comm, int *request, int *ierr) noexcept
{
  handle_MPI_IN_PLACE_f(sendbuf, recvbuf);
  *ierr = MPI_Iallgatherv(sendbuf, *sendcount, *sendtype, recvbuf, recvcounts,
                          displs, *recvtype, *comm, request);
}

void mpi_allgather(void *sendbuf, int *sendcount, int *sendtype,
                   void *recvbuf, int *recvcount, int *recvtype,
                   int *comm, int *ierr) noexcept
{
  handle_MPI_IN_PLACE_f(sendbuf, recvbuf);
  *ierr = MPI_Allgather(sendbuf, *sendcount, *sendtype, recvbuf, *recvcount,
                        *recvtype, *comm);
}

void mpi_gatherv(void *sendbuf, int *sendcount, int *sendtype,
                 void *recvbuf, int *recvcounts, int *displs,
                 int *recvtype, int *root, int *comm, int *ierr) noexcept
{
  handle_MPI_IN_PLACE_f(sendbuf, recvbuf);
  *ierr = MPI_Gatherv(sendbuf, *sendcount, *sendtype, recvbuf, recvcounts,
                      displs, *recvtype, *root, *comm);
}

void mpi_igatherv(void *sendbuf, int *sendcount, int *sendtype,
                  void *recvbuf, int *recvcounts, int *displs,
                  int *recvtype, int *root, int *comm, int *request, int *ierr) noexcept
{
  handle_MPI_IN_PLACE_f(sendbuf, recvbuf);
  *ierr = MPI_Igatherv(sendbuf, *sendcount, *sendtype, recvbuf, recvcounts,
                       displs, *recvtype, *root, *comm, request);
}

void mpi_gather(void *sendbuf, int *sendcount, int *sendtype,
                void *recvbuf, int *recvcount, int *recvtype,
                int *root, int *comm, int *ierr) noexcept
{
  handle_MPI_IN_PLACE_f(sendbuf, recvbuf);
  *ierr = MPI_Gather(sendbuf, *sendcount, *sendtype, recvbuf, *recvcount,
                     *recvtype, *root, *comm);
}

void mpi_igather(void *sendbuf, int *sendcount, int *sendtype,
                 void *recvbuf, int *recvcount, int *recvtype,
                 int *root, int *comm, int *request, int *ierr) noexcept
{
  handle_MPI_IN_PLACE_f(sendbuf, recvbuf);
  *ierr = MPI_Igather(sendbuf, *sendcount, *sendtype, recvbuf, *recvcount,
                      *recvtype, *root, *comm, request);
}

void mpi_scatterv(void *sendbuf, int *sendcounts, int *displs, int *sendtype,
                  void *recvbuf, int *recvcount, int *recvtype, int *root, int *comm, int *ierr) noexcept
{
  handle_MPI_IN_PLACE_f(sendbuf, recvbuf);
  *ierr = MPI_Scatterv(sendbuf, sendcounts, displs, *sendtype, recvbuf, *recvcount,
                       *recvtype, *root, *comm);
}

void mpi_iscatterv(void *sendbuf, int *sendcounts, int *displs, int *sendtype,
                   void *recvbuf, int *recvcount, int *recvtype, int *root, int *comm,
                   int *request, int *ierr) noexcept
{
  handle_MPI_IN_PLACE_f(sendbuf, recvbuf);
  *ierr = MPI_Iscatterv(sendbuf, sendcounts, displs, *sendtype, recvbuf, *recvcount,
                        *recvtype, *root, *comm, request);
}

void mpi_scatter(void *sendbuf, int *sendcount, int *sendtype,
                 void *recvbuf, int *recvcount, int *recvtype,
                 int *root, int *comm, int *ierr) noexcept
{
  handle_MPI_IN_PLACE_f(sendbuf, recvbuf);
  *ierr = MPI_Scatter(sendbuf, *sendcount, *sendtype, recvbuf, *recvcount,
                      *recvtype, *root, *comm);
}

void mpi_iscatter(void *sendbuf, int *sendcount, int *sendtype,
                 void *recvbuf, int *recvcount, int *recvtype,
                 int *root, int *comm, int *request, int *ierr) noexcept
{
  handle_MPI_IN_PLACE_f(sendbuf, recvbuf);
  *ierr = MPI_Iscatter(sendbuf, *sendcount, *sendtype, recvbuf, *recvcount,
                       *recvtype, *root, *comm, request);
}

void mpi_alltoallv(void *sendbuf, int *sendcounts, int *sdispls,
                   int *sendtype, void *recvbuf, int *recvcounts,
                   int *rdispls, int *recvtype, int *comm, int *ierr) noexcept
{
  handle_MPI_IN_PLACE_f(sendbuf, recvbuf);
  *ierr = MPI_Alltoallv(sendbuf, sendcounts, sdispls, *sendtype, recvbuf,
                        recvcounts, rdispls, *recvtype, *comm);
}

void mpi_ialltoallv(void *sendbuf, int *sendcounts, int *sdispls,
                    int *sendtype, void *recvbuf, int *recvcounts,
                    int *rdispls, int *recvtype, int *comm, int *request, int *ierr) noexcept
{
  handle_MPI_IN_PLACE_f(sendbuf, recvbuf);
  *ierr = MPI_Ialltoallv(sendbuf, sendcounts, sdispls, *sendtype, recvbuf,
                         recvcounts, rdispls, *recvtype, *comm, request);
}

void mpi_alltoallw(void *sendbuf, int *sendcounts, int *sdispls,
                   int *sendtypes, void *recvbuf, int *recvcounts, int *rdispls,
                   int *recvtypes, int *comm, int *ierr) noexcept
{
  handle_MPI_IN_PLACE_f(sendbuf, recvbuf);
  *ierr = MPI_Alltoallw(sendbuf, sendcounts, sdispls, sendtypes,
                        recvbuf, recvcounts, rdispls, recvtypes, *comm);
}

void mpi_ialltoallw(void *sendbuf, int *sendcounts, int *sdispls,
                    int *sendtypes, void *recvbuf, int *recvcounts, int *rdispls,
                    int *recvtypes, int *comm, int *request, int *ierr) noexcept
{
  handle_MPI_IN_PLACE_f(sendbuf, recvbuf);
  *ierr = MPI_Ialltoallw(sendbuf, sendcounts, sdispls, sendtypes,
                         recvbuf, recvcounts, rdispls, recvtypes,
                         *comm, request);
}

void mpi_alltoall(void *sendbuf, int *sendcount, int *sendtype,
                  void *recvbuf, int *recvcount, int *recvtype,
                  int *comm, int *ierr) noexcept
{
  handle_MPI_IN_PLACE_f(sendbuf, recvbuf);
  *ierr = MPI_Alltoall(sendbuf, *sendcount, *sendtype, recvbuf, *recvcount,
                       *recvtype, *comm);
}

void mpi_iallgather(void *sendbuf, int* sendcount, int* sendtype,
                    void *recvbuf, int* recvcount, int* recvtype,
                    int* comm, int* request, int* ierr) noexcept
{
  handle_MPI_IN_PLACE_f(sendbuf, recvbuf);
  *ierr = MPI_Iallgather(sendbuf, *sendcount, *sendtype, recvbuf, *recvcount,
                         *recvtype, *comm, (MPI_Request *)request);

}

void mpi_ialltoall(void *sendbuf, int* sendcount, int* sendtype,
                   void *recvbuf, int* recvcount, int* recvtype,
                   int* comm, int *request, int* ierr) noexcept
{
  handle_MPI_IN_PLACE_f(sendbuf, recvbuf);
  *ierr = MPI_Ialltoall(sendbuf, *sendcount, *sendtype,
                        recvbuf, *recvcount, *recvtype,
                        *comm, (MPI_Request *)request);
}

void mpi_ireduce(void *sendbuf, void *recvbuf, int* count, int* type,
                 int* op, int* root, int* comm, int *request, int* ierr) noexcept
{
  handle_MPI_IN_PLACE_f(sendbuf, recvbuf);
  *ierr = MPI_Ireduce(sendbuf, recvbuf, *count, *type,
                      *op, *root, *comm, (MPI_Request*) request);
}

void mpi_iallreduce(void *inbuf, void *outbuf, int* count, int* type,
                    int* op, int* comm, int *request, int* ierr) noexcept
{
  handle_MPI_IN_PLACE_f(inbuf, outbuf);
  *ierr = MPI_Iallreduce(inbuf, outbuf, *count, *type,
                         *op, *comm, (MPI_Request*) request);
}

void mpi_reduce_local(void *inbuf, void *outbuf, int *count, int *type,
                      int *op, int *ierr) noexcept
{
  *ierr = MPI_Reduce_local(inbuf, outbuf, *count, *type, *op);
}

void mpi_reduce_scatter_block(void* sendbuf, void* recvbuf, int *count,
                              int *type, int *op, int *comm, int *ierr) noexcept
{
  handle_MPI_IN_PLACE_f(sendbuf, recvbuf);
  *ierr = MPI_Reduce_scatter_block(sendbuf, recvbuf, *count, *type, *op, *comm);
}

void mpi_ireduce_scatter_block(void* sendbuf, void* recvbuf, int *count,
                               int *type, int *op, int *comm, int* request, int *ierr) noexcept
{
  handle_MPI_IN_PLACE_f(sendbuf, recvbuf);
  *ierr = MPI_Ireduce_scatter_block(sendbuf, recvbuf, *count, *type, *op, *comm, request);
}

void mpi_reduce_scatter(void *sendbuf, void *recvbuf, int *recvcounts,
                        int* datatype, int* op, int* comm, int* ierr) noexcept
{
  handle_MPI_IN_PLACE_f(sendbuf, recvbuf);
  *ierr = MPI_Reduce_scatter(sendbuf, recvbuf, recvcounts,
                             *datatype, *op, *comm);
}

void mpi_ireduce_scatter(void *sendbuf, void *recvbuf, int *recvcounts,
                         int* datatype, int* op, int* comm, int* request, int* ierr) noexcept
{
  handle_MPI_IN_PLACE_f(sendbuf, recvbuf);
  *ierr = MPI_Ireduce_scatter(sendbuf, recvbuf, recvcounts,
                              *datatype, *op, *comm, request);
}

void mpi_scan(void* sendbuf, void* recvbuf, int* count, int* datatype, int* op, int* comm, int* ierr) noexcept
{
  handle_MPI_IN_PLACE_f(sendbuf,recvbuf);
  *ierr = MPI_Scan(sendbuf,recvbuf,*count,*datatype,*op,*comm );
}

void mpi_iscan(void* sendbuf, void* recvbuf, int* count, int* datatype, int* op, int* comm, int* request, int* ierr) noexcept
{
  handle_MPI_IN_PLACE_f(sendbuf,recvbuf);
  *ierr = MPI_Iscan(sendbuf,recvbuf,*count,*datatype,*op,*comm,request);
}

void mpi_exscan(void* sendbuf, void* recvbuf, int* count, int* datatype, int* op, int* comm, int* ierr) noexcept
{
  handle_MPI_IN_PLACE_f(sendbuf, recvbuf);
  *ierr = MPI_Exscan(sendbuf,recvbuf,*count,*datatype,*op,*comm);
}

void mpi_iexscan(void* sendbuf, void* recvbuf, int* count, int* datatype, int* op, int* comm, int* request, int* ierr) noexcept
{
  handle_MPI_IN_PLACE_f(sendbuf, recvbuf);
  *ierr = MPI_Iexscan(sendbuf,recvbuf,*count,*datatype,*op,*comm,request);
}

void mpi_neighbor_alltoall(void* sendbuf, int *sendcount, int *sendtype,
                           void *recvbuf, int *recvcount, int *recvtype,
                           int *comm, int *ierr) noexcept
{
  *ierr = MPI_Neighbor_alltoall(sendbuf, *sendcount, *sendtype, recvbuf,
                                *recvcount, *recvtype, *comm);
}

void mpi_ineighbor_alltoall(void* sendbuf, int *sendcount, int *sendtype,
                            void *recvbuf, int *recvcount, int *recvtype,
                            int *comm, int *request, int *ierr) noexcept
{
  *ierr = MPI_Ineighbor_alltoall(sendbuf, *sendcount, *sendtype, recvbuf,
                                 *recvcount, *recvtype, *comm, request);
}

void mpi_neighbor_alltoallv(void* sendbuf, int *sendcounts, int *sdispls,
                            int *sendtype, void *recvbuf, int *recvcounts,
                            int *rdispls, int *recvtype, int *comm,
                            int *ierr) noexcept
{
  *ierr = MPI_Neighbor_alltoallv(sendbuf, sendcounts, sdispls, *sendtype,
                                 recvbuf, recvcounts, rdispls, *recvtype,
                                 *comm);
}

void mpi_ineighbor_alltoallv(void* sendbuf, int *sendcounts, int *sdispls,
                             int *sendtype, void *recvbuf, int *recvcounts,
                             int *rdispls, int *recvtype, int *comm,
                             int *request, int *ierr) noexcept
{
  *ierr = MPI_Ineighbor_alltoallv(sendbuf, sendcounts, sdispls, *sendtype,
                                  recvbuf, recvcounts, rdispls, *recvtype,
                                  *comm, request);
}

void mpi_neighbor_alltoallw(void* sendbuf, int *sendcounts, MPI_Aint *sdispls,
                            int *sendtypes, void *recvbuf, int *recvcounts,
                            MPI_Aint *rdispls, int *recvtypes, int *comm,
                            int *ierr) noexcept
{
  *ierr = MPI_Neighbor_alltoallw(sendbuf, sendcounts, sdispls, sendtypes,
                                 recvbuf, recvcounts, rdispls, recvtypes,
                                 *comm);
}

void mpi_ineighbor_alltoallw(void* sendbuf, int *sendcounts, MPI_Aint *sdispls,
                             int *sendtypes, void *recvbuf, int *recvcounts,
                             MPI_Aint *rdispls, int *recvtypes, int *comm,
                             int *request, int *ierr) noexcept
{
  *ierr = MPI_Ineighbor_alltoallw(sendbuf, sendcounts, sdispls, sendtypes,
                                  recvbuf, recvcounts, rdispls, recvtypes,
                                  *comm, request);
}

void mpi_neighbor_allgather(void* sendbuf, int *sendcount, int *sendtype,
                            void *recvbuf, int *recvcount, int *recvtype,
                            int *comm, int *ierr) noexcept
{
  *ierr = MPI_Neighbor_allgather(sendbuf, *sendcount, *sendtype, recvbuf,
                                 *recvcount, *recvtype, *comm);
}

void mpi_ineighbor_allgather(void* sendbuf, int *sendcount, int *sendtype,
                             void *recvbuf, int *recvcount, int *recvtype,
                             int *comm, int *request, int *ierr) noexcept
{
  *ierr = MPI_Ineighbor_allgather(sendbuf, *sendcount, *sendtype, recvbuf,
                                  *recvcount, *recvtype, *comm, request);
}

void mpi_neighbor_allgatherv(void* sendbuf, int *sendcount, int *sendtype,
                             void *recvbuf, int *recvcounts, int *displs,
                             int *recvtype, int *comm, int *ierr) noexcept
{
  *ierr = MPI_Neighbor_allgatherv(sendbuf, *sendcount, *sendtype, recvbuf,
                                  recvcounts, displs, *recvtype, *comm);
}

void mpi_ineighbor_allgatherv(void* sendbuf, int *sendcount, int *sendtype,
                              void *recvbuf, int *recvcounts, int *displs,
                              int *recvtype, int *comm, int *request, int *ierr) noexcept
{
  *ierr = MPI_Ineighbor_allgatherv(sendbuf, *sendcount, *sendtype, recvbuf,
                                   recvcounts, displs, *recvtype, *comm, request);
}

void mpi_op_create(void* function, int* commute, int* op, int* ierr) noexcept {
  *ierr = MPI_Op_create((MPI_User_function *)function, *commute, op);
}

void mpi_op_free(int* op, int* ierr) noexcept {
  *ierr = MPI_Op_free(op);
}

void mpi_op_commutative(int* op, int* commute, int* ierr) noexcept {
  *ierr = MPI_Op_commutative(*op, commute);
}

void mpi_comm_dup(int *comm, int *newcomm, int *ierr) noexcept
{
  *ierr = MPI_Comm_dup(*comm, newcomm);
}

void mpi_comm_idup(int *comm, int *newcomm, int* request, int *ierr) noexcept
{
  *ierr = MPI_Comm_idup(*comm, newcomm, request);
}

void mpi_comm_dup_with_info(int *comm, int *info, int *newcomm, int *ierr) noexcept
{
  *ierr = MPI_Comm_dup_with_info(*comm, *info, newcomm);
}

void mpi_comm_idup_with_info(int *comm, int *info, int *newcomm, int *request, int *ierr) noexcept
{
  *ierr = MPI_Comm_idup_with_info(*comm, *info, newcomm, request);
}

void mpi_comm_split(int* src, int* color, int* key, int *dest, int *ierr) noexcept
{
  *ierr = MPI_Comm_split(*src, *color, *key, dest);
}

void mpi_comm_split_type(int* src, int* split_type, int* key, int* info, int* dest, int* ierr) noexcept
{
  *ierr = MPI_Comm_split_type(*src, *split_type, *key, *info, dest);
}

void mpi_comm_free(int *comm, int *ierr) noexcept
{
  *ierr = MPI_Comm_free(comm);
}

void mpi_comm_test_inter(int* comm, int* flag, int* ierr) noexcept
{
  *ierr = MPI_Comm_test_inter(*comm, flag);
}

void mpi_cart_create(int* comm_old, int* ndims, int *dims, int *periods,
                     int* reorder, int* comm_cart, int* ierr) noexcept
{
  *ierr = MPI_Cart_create(*comm_old, *ndims, dims, periods, *reorder, comm_cart);
}

void mpi_graph_create(int* comm_old, int* nnodes, int *index, int *edges,
                      int* reorder, int* comm_graph, int* ierr) noexcept
{
  *ierr = MPI_Graph_create(*comm_old, *nnodes, index, edges, *reorder, comm_graph);
}

void mpi_dist_graph_create_adjacent(int* comm_old, int* indegree, int* sources, int* sourceweights,
                                    int* outdegree, int* destinations, int* destweights, int* info,
                                    int* reorder, int* comm_dist_graph, int* ierr) noexcept
{
  *ierr = MPI_Dist_graph_create_adjacent(*comm_old, *indegree, sources, sourceweights, *outdegree,
                                         destinations, destweights, *info, *reorder, comm_dist_graph);
}

void mpi_dist_graph_create(int* comm_old, int* n, int* sources, int* degrees,
                           int* destinations, int* weights, int* info, int* reorder,
                           int* comm_dist_graph, int* ierr) noexcept
{
  *ierr = MPI_Dist_graph_create(*comm_old, *n, sources, degrees, destinations,
                                weights, *info, *reorder, comm_dist_graph);
}

void mpi_topo_test(int* comm, int *status, int* ierr) noexcept
{
  *ierr = MPI_Topo_test(*comm, status);
}

void mpi_cart_map(int* comm, int* ndims, int *dims, int *periods,
                 int *newrank, int* ierr) noexcept
{
  *ierr = MPI_Cart_map(*comm, *ndims, dims, periods, newrank);
}

void mpi_graph_map(int* comm, int* nnodes, int *index, int *edges,
                   int *newrank, int* ierr) noexcept
{
  *ierr = MPI_Graph_map(*comm, *nnodes, index, edges, newrank);
}

void mpi_cartdim_get(int* comm, int *ndims, int* ierr) noexcept
{
  *ierr = MPI_Cartdim_get(*comm, ndims);
}

void mpi_cart_get(int* comm, int* maxdims, int *dims, int *periods,
                  int *coords, int* ierr) noexcept
{
  *ierr = MPI_Cart_get(*comm, *maxdims, dims, periods, coords);
}

void mpi_cart_rank(int* comm, int *coords, int *rank, int* ierr) noexcept
{
  *ierr = MPI_Cart_rank(*comm, coords, rank);
}

void mpi_cart_coords(int* comm, int* rank, int* maxdims, int *coords, int* ierr) noexcept
{
  *ierr = MPI_Cart_coords(*comm, *rank, *maxdims, coords);
}

void mpi_cart_shift(int* comm, int* direction, int* disp, int *rank_source,
                    int *rank_dest, int* ierr) noexcept
{
  *ierr = MPI_Cart_shift(*comm, *direction, *disp, rank_source, rank_dest);
}

void mpi_graphdims_get(int* comm, int *nnodes, int *nedges, int* ierr) noexcept
{
  *ierr = MPI_Graphdims_get(*comm, nnodes, nedges);
}

void mpi_graph_get(int* comm, int *maxindex, int *maxedges, int *index,
                   int *edges, int* ierr) noexcept
{
  *ierr = MPI_Graph_get(*comm, *maxindex, *maxedges, index, edges);
}

void mpi_graph_neighbors_count(int* comm, int *rank, int *nneighbors, int* ierr) noexcept
{
  *ierr = MPI_Graph_neighbors_count(*comm, *rank, nneighbors);
}

void mpi_graph_neighbors(int* comm, int *rank, int *maxneighbors,
                         int *neighbors, int* ierr) noexcept
{
  *ierr = MPI_Graph_neighbors(*comm, *rank, *maxneighbors, neighbors);
}

void mpi_dist_graph_neighbors_count(int* comm_dist_graph, int* indegree, int* outdegree,
                                    int* weighted, int* ierr) noexcept
{
  *ierr = MPI_Dist_graph_neighbors_count(*comm_dist_graph, indegree, outdegree, weighted);
}

void mpi_dist_graph_neighbors(int* comm_dist_graph, int* maxindegree, int* sources,
                              int* sourceweights, int* maxoutdegree,
                              int* destinations, int* destweights, int* ierr) noexcept
{
  *ierr = MPI_Dist_graph_neighbors(*comm_dist_graph, *maxindegree, sources,
                                   sourceweights, *maxoutdegree, destinations, destweights);
}

void mpi_dims_create(int *nnodes, int *ndims, int *dims, int* ierr) noexcept
{
  *ierr = MPI_Dims_create(*nnodes, *ndims, dims);
}

void mpi_cart_sub(int* comm, int *remain_dims, int* newcomm, int* ierr) noexcept
{
  *ierr = MPI_Cart_sub(*comm, remain_dims, newcomm);
}

void mpi_get_version(int *version, int *subversion, int *ierr) noexcept
{
  *ierr = MPI_Get_version(version, subversion);
}

void mpi_get_library_version(char* name, int *resultlen, int *ierr) noexcept
{
  char tmpName[MPI_MAX_LIBRARY_VERSION_STRING];

  *ierr = MPI_Get_library_version(tmpName, resultlen);

  if (*ierr == MPI_SUCCESS)
    ampif_str_c2f(name, tmpName, MPI_MAX_LIBRARY_VERSION_STRING);
}

void mpi_get_processor_name(char* name, int *resultlen, int *ierr) noexcept
{
  char tmpName[MPI_MAX_PROCESSOR_NAME];

  *ierr = MPI_Get_processor_name(tmpName, resultlen);

  if (*ierr == MPI_SUCCESS)
    ampif_str_c2f(name, tmpName, MPI_MAX_PROCESSOR_NAME);
}

void mpi_comm_create_errhandler(void (*function)(MPI_Comm*,int*,...), int *errhandler, int *ierr) noexcept
{
  *ierr = MPI_Comm_create_errhandler(function, errhandler);
}

void mpi_comm_set_errhandler(int* comm, int* errhandler, int *ierr) noexcept
{
  *ierr = MPI_Comm_set_errhandler(*comm, *errhandler);
}

void mpi_comm_get_errhandler(int* comm, int *errhandler, int *ierr) noexcept
{
  *ierr = MPI_Comm_get_errhandler(*comm, errhandler);
}

void mpi_comm_free_errhandler(int *errhandler, int *ierr) noexcept
{
  *ierr = MPI_Comm_free_errhandler(errhandler);
}

void mpi_file_create_errhandler(void (*function)(MPI_File*,int*,...), int *errhandler, int *ierr) noexcept
{
  *ierr = MPI_File_create_errhandler(function, errhandler);
}

#if !CMK_AMPI_WITH_ROMIO
// Disable ROMIO's get_errhf.c and set_errhf.c if enabling these.
void mpi_file_set_errhandler(MPI_File* file, int* errhandler, int *ierr) noexcept
{
  *ierr = MPI_File_set_errhandler(*file, *errhandler);
}

void mpi_file_get_errhandler(MPI_File* file, int *errhandler, int *ierr) noexcept
{
  *ierr = MPI_File_get_errhandler(*file, errhandler);
}
#endif

void mpi_errhandler_create(void (*function)(MPI_Comm*,int*,...), int *errhandler, int *ierr) noexcept
{
  *ierr = MPI_Errhandler_create(function, errhandler);
}

void mpi_errhandler_set(int* comm, int* errhandler, int *ierr) noexcept
{
  *ierr = MPI_Errhandler_set(*comm, *errhandler);
}

void mpi_errhandler_get(int* comm, int *errhandler, int *ierr) noexcept
{
  *ierr = MPI_Errhandler_get(*comm, errhandler);
}

void mpi_errhandler_free(int *errhandler, int *ierr) noexcept
{
  *ierr = MPI_Errhandler_free(errhandler);
}

void mpi_add_error_code(int *errorclass, int *errorcode, int *ierr) noexcept
{
  *ierr = MPI_Add_error_code(*errorcode, errorcode);
}

void mpi_add_error_class(int *errorclass, int *ierr) noexcept
{
  *ierr = MPI_Add_error_class(errorclass);
}

void ampif_add_error_string(int *errorcode, const char *errorstring, int* elen, int *ierr) noexcept
{
  char tmpErrorstring[MPI_MAX_ERROR_STRING];
  ampif_str_f2c(tmpErrorstring, errorstring, *elen);

  *ierr = MPI_Add_error_string(*errorcode, tmpErrorstring);
}

void mpi_error_class(int* errorcode, int *errorclass, int *ierr) noexcept
{
  *ierr = MPI_Error_class(*errorcode, errorclass);
}

void mpi_error_string(int* errorcode, char *string, int *resultlen, int *ierr) noexcept
{
  char tmpString[MPI_MAX_ERROR_STRING];

  *ierr = MPI_Error_string(*errorcode, tmpString, resultlen);

  if (*ierr == MPI_SUCCESS)
    ampif_str_c2f(string, tmpString, MPI_MAX_ERROR_STRING);
}

void mpi_group_size(int* group, int* size, int* ierror) noexcept
{
  *ierror = MPI_Group_size(*group, size);
}

void mpi_group_rank(int* group, int* rank, int* ierror) noexcept
{
  *ierror = MPI_Group_rank(*group, rank);
}

void mpi_group_translate_ranks(int* group1, int* n, int* ranks1, int* group2, int* ranks2, int* ierror) noexcept
{
  *ierror = MPI_Group_translate_ranks(*group1, *n, ranks1, *group2, ranks2);
}

void mpi_group_compare(int* group1, int* group2, int* result, int* ierror) noexcept
{
  *ierror = MPI_Group_compare(*group1, *group2, result);
}

void mpi_comm_group(int* comm, int* group, int* ierror) noexcept
{
  *ierror = MPI_Comm_group(*comm, group);
}

void mpi_group_union(int* group1, int* group2, int* newgroup, int* ierror) noexcept
{
  *ierror = MPI_Group_union(*group1, *group2, newgroup);
}

void mpi_group_intersection(int* group1, int* group2, int* newgroup, int* ierror) noexcept
{
  *ierror = MPI_Group_intersection(*group1, *group2, newgroup);
}

void mpi_group_difference(int* group1, int* group2, int* newgroup, int* ierror) noexcept
{
  *ierror = MPI_Group_difference(*group1, *group2, newgroup);
}

void mpi_group_incl(int* group, int* n, int* ranks, int* newgroup, int* ierror) noexcept
{
  *ierror = MPI_Group_incl(*group, *n, ranks, newgroup);
}

void mpi_group_excl(int* group, int* n, int* ranks, int* newgroup, int* ierror) noexcept
{
  *ierror = MPI_Group_excl(*group, *n, ranks, newgroup);
}

void mpi_group_range_incl(int* group, int* n, int ranges[][3], int* newgroup, int* ierror) noexcept
{
  *ierror = MPI_Group_range_incl(*group, *n, ranges, newgroup);
}

void mpi_group_range_excl(int* group,int*  n, int ranges[][3], int* newgroup, int* ierror) noexcept
{
  *ierror = MPI_Group_range_excl(*group, *n, ranges, newgroup);
}

void mpi_group_free(int* group, int* ierror) noexcept
{
  *ierror = MPI_Group_free(group);
}

void mpi_comm_create(int* comm, int* group, int* newcomm, int* ierror) noexcept
{
  *ierror = MPI_Comm_create(*comm, *group, newcomm);
}

void mpi_comm_create_group(int* comm, int* group, int* tag, int* newcomm, int *ierror) noexcept
{
  *ierror = MPI_Comm_create_group(*comm, *group, *tag, newcomm);
}

void mpi_abort(int *comm, int *errorcode, int *ierr) noexcept
{
  *ierr = MPI_Abort(*comm, *errorcode);
}

void mpi_get_count(int *sts, int *dtype, int *cnt, int *ierr) noexcept
{
  MPI_Status* s = handle_MPI_STATUS_IGNORE(sts);
  *ierr = MPI_Get_count(s, *dtype, cnt);
}

void mpi_comm_remote_size(int *comm, int *size, int *ierr) noexcept
{
  *ierr = MPI_Comm_remote_size(*comm, size);
}

void mpi_comm_remote_group(int *comm, int *group, int *ierr) noexcept
{
  *ierr = MPI_Comm_remote_group(*comm, group);
}

void mpi_intercomm_create(int *local_comm, int *local_leader, int *peer_comm, int *remote_leader,
                          int *tag, int *newintercomm, int *ierr) noexcept
{
  *ierr = MPI_Intercomm_create(*local_comm, *local_leader, *peer_comm, *remote_leader,
                               *tag, newintercomm);
}

void mpi_intercomm_merge(int *intercomm, int *high, int *newintracomm, int *ierr) noexcept
{
  *ierr = MPI_Intercomm_merge(*intercomm, *high, newintracomm);
}

void ampif_comm_set_name(int *comm, const char *comm_name, int* nlen, int *ierr) noexcept
{
  char tmpName[MPI_MAX_OBJECT_NAME];
  ampif_str_f2c(tmpName, comm_name, *nlen);

  *ierr = MPI_Comm_set_name(*comm, tmpName);
}

void mpi_comm_get_name(int *comm, char *comm_name, int *resultlen, int *ierr) noexcept
{
  char tmpName[MPI_MAX_OBJECT_NAME];

  *ierr = MPI_Comm_get_name(*comm, tmpName, resultlen);

  if (*ierr == MPI_SUCCESS)
    ampif_str_c2f(comm_name, tmpName, MPI_MAX_OBJECT_NAME);
}

void mpi_comm_set_info(int *comm, int *info, int *ierr) noexcept
{
  *ierr = MPI_Comm_set_info(*comm, *info);
}

void mpi_comm_get_info(int *comm, int *info, int *ierr) noexcept
{
  *ierr = MPI_Comm_get_info(*comm, info);
}

void mpi_comm_create_keyval(MPI_Comm_copy_attr_function *copy_fn,
                            MPI_Comm_delete_attr_function *delete_fn, int *keyval,
                            void* extra_state, int *ierr) noexcept
{
  *ierr = MPI_Comm_create_keyval(copy_fn, delete_fn, keyval, extra_state);
}

void mpi_comm_free_keyval(int *keyval, int *ierr) noexcept
{
  *ierr = MPI_Comm_free_keyval(keyval);
}

void mpi_comm_set_attr(int *comm, int *keyval, void* attribute_val, int *ierr) noexcept
{
  *ierr = MPI_Comm_set_attr(*comm, *keyval, attribute_val);
}

void mpi_comm_get_attr(int *comm, int *keyval, void *attribute_val, int *flag, int *ierr) noexcept
{
  int * result;
  *ierr = MPI_Comm_get_attr(*comm, *keyval, &result, flag);
  *(intptr_t *)attribute_val = *result;
}

void mpi_comm_delete_attr(int *comm, int *keyval, int *ierr) noexcept
{
  *ierr = MPI_Comm_delete_attr(*comm, *keyval);
}

void mpi_keyval_create(MPI_Copy_function *copy_fn, MPI_Delete_function *delete_fn,
                       int *keyval, void* extra_state, int *ierr) noexcept
{
  *ierr = MPI_Keyval_create(copy_fn, delete_fn, keyval, extra_state);
}

void mpi_keyval_free(int *keyval, int *ierr) noexcept
{
  *ierr = MPI_Keyval_free(keyval);
}

void mpi_attr_put(int *comm, int *keyval, void* attribute_val, int *ierr) noexcept
{
  *ierr = MPI_Attr_put(*comm, *keyval, attribute_val);
}

void mpi_attr_get(int *comm, int *keyval, void *attribute_val, int *flag, int *ierr) noexcept
{
  int * result;
  *ierr = MPI_Attr_get(*comm, *keyval, &result, flag);
  *(intptr_t *)attribute_val = *result;
}

void mpi_attr_delete(int *comm, int *keyval, int *ierr) noexcept
{
  *ierr = MPI_Attr_delete(*comm, *keyval);
}

void mpi_type_get_envelope(int *datatype, int *num_integers, int *num_addresses,
                          int *num_datatypes, int *combiner, int *ierr) noexcept
{
 *ierr = MPI_Type_get_envelope(*datatype, num_integers, num_addresses, num_datatypes, combiner);
}

void mpi_type_get_contents(int *datatype, int *max_integers, int *max_addresses,
                           int *max_datatypes, int array_of_integers[], MPI_Aint array_of_addresses[],
                           int array_of_datatypes[], int *ierr) noexcept
{
  *ierr = MPI_Type_get_contents(*datatype, *max_integers, *max_addresses, *max_datatypes,
                                array_of_integers, array_of_addresses, array_of_datatypes);
}

void mpi_win_create(void *base, MPI_Aint *size, int *disp_unit,
                    int *info, int *comm, MPI_Win *newwin, int *ierr) noexcept
{
  *ierr = MPI_Win_create(base, *size, *disp_unit, *info, *comm, newwin);
}

void mpi_win_allocate(MPI_Aint *size, int *disp_unit,
                      int *info, int *comm, void *base, MPI_Win *win, int *ierr) noexcept
{
  *ierr = MPI_Win_allocate(*size, *disp_unit, *info, *comm, base, win);
}

void mpi_win_free(int *win, int *ierr) noexcept
{
  *ierr = MPI_Win_free(win);
}

void mpi_win_create_errhandler(MPI_Win_errhandler_function *win_errhandler_fn,
                               int *errhandler, int *ierr) noexcept
{
  *ierr = MPI_Win_create_errhandler(win_errhandler_fn, errhandler);
}

void mpi_win_call_errhandler(MPI_Win *win, int *errcode, int *ierr) noexcept
{
  *ierr = MPI_Win_call_errhandler(*win, *errcode);
}

void mpi_win_get_errhandler(MPI_Win *win, int *errhandler, int *ierr) noexcept
{
  *ierr = MPI_Win_get_errhandler(*win, errhandler);
}

void mpi_win_set_errhandler(MPI_Win *win, int *errhandler, int *ierr) noexcept
{
  *ierr = MPI_Win_set_errhandler(*win, *errhandler);
}

void mpi_win_create_keyval(MPI_Win_copy_attr_function *copy_fn,
                           MPI_Win_delete_attr_function *delete_fn,
                           int *keyval, void *extra_state, int *ierr) noexcept
{
  *ierr = MPI_Win_create_keyval(copy_fn, delete_fn, keyval, extra_state);
}

void mpi_win_free_keyval(int *keyval, int *ierr) noexcept
{
  *ierr = MPI_Win_free_keyval(keyval);
}

void mpi_win_delete_attr(int *win, int *key, int *ierr) noexcept
{
  *ierr = MPI_Win_delete_attr(*win, *key);
}

void mpi_win_get_attr(int *win, int *win_keyval, void *attribute_val, int *flag,
                      int *ierr) noexcept
{
  int * result;
  *ierr = MPI_Win_get_attr(*win, *win_keyval, &result, flag);
  *(intptr_t *)attribute_val = *result;
}

void mpi_win_set_attr(int *win, int *win_keyval, void *attribute_val, int *ierr) noexcept
{
  *ierr = MPI_Win_set_attr(*win, *win_keyval, attribute_val);
}

void mpi_win_get_group(int *win, int *group, int *ierr) noexcept
{
  *ierr = MPI_Win_get_group(*win, group);
}

void ampif_win_set_name(int *win, const char *name, int* nlen, int *ierr) noexcept
{
  char tmpName[MPI_MAX_OBJECT_NAME];
  ampif_str_f2c(tmpName, name, *nlen);

  *ierr = MPI_Win_set_name(*win, tmpName);
}

void mpi_win_get_name(int *win, char *name, int *length, int *ierr) noexcept
{
  char tmpName[MPI_MAX_OBJECT_NAME];

  *ierr = MPI_Win_get_name(*win, tmpName, length);

  if (*ierr == MPI_SUCCESS)
    ampif_str_c2f(name, tmpName, MPI_MAX_OBJECT_NAME);
}

void mpi_win_set_info(int *win, int *info, int *ierr) noexcept
{
  *ierr = MPI_Win_set_info(*win, *info);
}

void mpi_win_get_info(int *win, int *info, int *ierr) noexcept
{
  *ierr = MPI_Win_get_info(*win, info);
}

void mpi_win_fence(int *assertion, int *win, int *ierr) noexcept
{
  *ierr = MPI_Win_fence(*assertion, *win);
}

void mpi_win_lock(int *lock_type, int *rank, int *assert, int *win, int *ierr) noexcept
{
  *ierr = MPI_Win_lock(*lock_type, *rank, *assert, *win);
}

void mpi_win_lock_all(int *assert, int *win, int *ierr) noexcept
{
  *ierr = MPI_Win_lock_all(*assert, *win);
}

void mpi_win_unlock(int *rank, int *win, int *ierr) noexcept
{
  *ierr = MPI_Win_unlock(*rank, *win);
}

void mpi_win_unlock_all(int *win, int *ierr) noexcept
{
  *ierr = MPI_Win_unlock_all(*win);
}

void mpi_win_post(int *group, int *assertion, int *win, int *ierr) noexcept
{
  *ierr = MPI_Win_post(*group, *assertion, *win);
}

void mpi_win_wait(int *win, int *ierr) noexcept
{
  *ierr = MPI_Win_wait(*win);
}

void mpi_win_start(int *group, int *assertion, int *win, int *ierr) noexcept
{
  *ierr = MPI_Win_start(*group, *assertion, *win);
}

void mpi_win_complete(int *win, int *ierr) noexcept
{
  *ierr = MPI_Win_complete(*win);
}

void mpi_win_test(int *win, int *flag, int *ierr) noexcept
{
  *ierr = MPI_Win_test(*win,flag);
}

void mpi_alloc_mem(MPI_Aint *size, int *info, void *baseptr, int *ierr) noexcept
{
  *ierr = MPI_Alloc_mem(*size, *info, baseptr);
}

void mpi_free_mem(void *base, int *ierr) noexcept
{
  *ierr = MPI_Free_mem(base);
}

void mpi_put(void *orgaddr, int *orgcnt, int *orgtype, int *rank,
             MPI_Aint *targdisp, int *targcnt, int *targtype, int *win, int *ierr) noexcept
{
  *ierr = MPI_Put(orgaddr, *orgcnt, *orgtype, *rank, *targdisp, *targcnt, *targtype, *win);
}

void mpi_get(void *orgaddr, int *orgcnt, int *orgtype, int *rank,
             MPI_Aint *targdisp, int *targcnt, int *targtype, int *win, int *ierr) noexcept
{
  *ierr = MPI_Get(orgaddr, *orgcnt, *orgtype, *rank, *targdisp, *targcnt, *targtype, *win);
}

void mpi_accumulate(void *orgaddr, int *orgcnt, int *orgtype, int *rank,
                    MPI_Aint *targdisp, int *targcnt, int *targtype,
                    int *op, int *win, int *ierr) noexcept
{
  *ierr = MPI_Accumulate(orgaddr, *orgcnt, *orgtype, *rank, *targdisp, *targcnt, *targtype, *op, *win);
}

void mpi_get_accumulate(void *orgaddr, int *orgcnt, int *orgtype, void *resaddr,
                        int *rescnt, int *restype, int *rank,
                        MPI_Aint *targdisp, int *targcnt, int *targtype,
                        int *op, int *win, int *ierr) noexcept
{
  *ierr = MPI_Get_accumulate(orgaddr, *orgcnt, *orgtype, resaddr, *rescnt, *restype,
                             *rank, *targdisp, *targcnt, *targtype, *op, *win);
}

void mpi_rput(void *orgaddr, int *orgcnt, int *orgtype, int *rank,
              MPI_Aint *targdisp, int *targcnt, int *targtype, int *win,
              int *request, int *ierr) noexcept
{
  *ierr = MPI_Rput(orgaddr, *orgcnt, *orgtype, *rank, *targdisp,
                   *targcnt, *targtype, *win, request);
}

void mpi_rget(void *orgaddr, int *orgcnt, int *orgtype, int *rank,
              MPI_Aint *targdisp, int *targcnt, int *targtype, int *win,
              int *request, int *ierr) noexcept
{
  *ierr = MPI_Rget(orgaddr, *orgcnt, *orgtype, *rank, *targdisp,
                   *targcnt, *targtype, *win, request);
}

void mpi_raccumulate(void *orgaddr, int *orgcnt, int *orgtype, int *rank,
                     MPI_Aint *targdisp, int *targcnt, int *targtype,
                     int *op, int *win, int *request, int *ierr) noexcept
{
  *ierr = MPI_Raccumulate(orgaddr, *orgcnt, *orgtype, *rank, *targdisp,
                          *targcnt, *targtype, *op, *win, request);
}

void mpi_rget_accumulate(void *orgaddr, int *orgcnt, int *orgtype, void *resaddr,
                         int *rescnt, int *restype, int *rank,
                         MPI_Aint *targdisp, int *targcnt, int *targtype,
                         int *op, int *win, int *request, int *ierr) noexcept
{
  *ierr = MPI_Rget_accumulate(orgaddr, *orgcnt, *orgtype, resaddr, *rescnt, *restype,
                              *rank, *targdisp, *targcnt, *targtype, *op, *win, request);
}

void mpi_fetch_and_op(void *orgaddr, void *resaddr, int *type, int *rank,
                      MPI_Aint *targdisp, int *op, int *win, int *ierr) noexcept
{
  *ierr = MPI_Fetch_and_op(orgaddr, resaddr, *type, *rank, *targdisp, *op, *win);
}

void mpi_compare_and_swap(void *orgaddr, void *compaddr, void *resaddr,
                          int *type, int *rank, MPI_Aint *targdisp,
                          MPI_Win *win, int *ierr) noexcept
{
  *ierr = MPI_Compare_and_swap(orgaddr, compaddr, resaddr, *type, *rank, *targdisp, *win);
}

void mpi_info_create(int* info, int* ierr) noexcept
{
  *ierr = MPI_Info_create(info);
}

void ampif_info_set(int* info, const char *key, const char *value,
                    int *klen, int *vlen, int *ierr) noexcept
{
  char tmpKey[MPI_MAX_INFO_KEY];
  ampif_str_f2c(tmpKey, key, *klen);

  char tmpValue[MPI_MAX_INFO_VAL];
  ampif_str_f2c(tmpValue, value, *vlen);

  *ierr = MPI_Info_set(*info, tmpKey, tmpValue);
}

void ampif_info_delete(int* info, const char* key, int* klen, int* ierr) noexcept
{
  char tmpKey[MPI_MAX_INFO_KEY];
  ampif_str_f2c(tmpKey, key, *klen);

  *ierr = MPI_Info_delete(*info, tmpKey);
}

void ampif_info_get(int* info, const char *key, int* valuelen, char *value, int *flag,
                    int* klen, int* ierr) noexcept
{
  char tmpKey[MPI_MAX_INFO_KEY];
  ampif_str_f2c(tmpKey, key, *klen);

  std::vector<char> tmpValue(*valuelen);

  *ierr = MPI_Info_get(*info, tmpKey, *valuelen, tmpValue.data(), flag);

  if (*ierr == MPI_SUCCESS)
    ampif_str_c2f(value, tmpValue.data(), *valuelen);
}

void ampif_info_get_valuelen(int* info, const char *key, int *valuelen, int *flag,
                             int *klen, int* ierr) noexcept
{
  char tmpKey[MPI_MAX_INFO_KEY];
  ampif_str_f2c(tmpKey, key, *klen);

  *ierr = MPI_Info_get_valuelen(*info, tmpKey, valuelen, flag);
}

void mpi_info_get_nkeys(int* info, int *nkeys, int* ierr) noexcept
{
  *ierr = MPI_Info_get_nkeys(*info, nkeys);
}

void mpi_info_get_nthkey(int* info, int *n, char *key, int* ierr) noexcept
{
  char tmpKey[MPI_MAX_INFO_KEY];

  *ierr = MPI_Info_get_nthkey(*info, *n, tmpKey);

  if (*ierr == MPI_SUCCESS)
    ampif_str_c2f(key, tmpKey, MPI_MAX_INFO_KEY);
}

void mpi_info_dup(int* info, int* newinfo, int* ierr) noexcept
{
  *ierr = MPI_Info_dup(*info, newinfo);
}

void mpi_info_free(int* info, int* ierr) noexcept
{
  *ierr = MPI_Info_free(info);
}

void mpi_pcontrol(int *level) noexcept
{
  MPI_Pcontrol(*level);
}

/* Extensions needed by ROMIO */
void mpir_status_set_bytes(int *status, int* datatype, MPI_Count *nbytes, int* ierr) noexcept
{
  MPI_Status* s = handle_MPI_STATUS_IGNORE(status);
  *ierr = MPIR_Status_set_bytes(s, *datatype, *nbytes);
}

/* AMPI Extensions */
void ampi_migrate(int *hints, int *ierr) noexcept
{
  *ierr = AMPI_Migrate(*hints);
}

void ampi_load_start_measure(int *ierr) noexcept
{
  *ierr = AMPI_Load_start_measure();
}

void ampi_load_stop_measure(int *ierr) noexcept
{
  *ierr = AMPI_Load_stop_measure();
}

void ampi_load_reset_measure(int *ierr) noexcept
{
  *ierr = AMPI_Load_reset_measure();
}

void ampi_load_set_value(double *value, int *ierr) noexcept
{
  *ierr = AMPI_Load_set_value(*value);
}

void ampi_load_set_phase(int *phase, int *ierr) noexcept
{
  *ierr = AMPI_Load_set_phase(*phase);
}

void ampi_migrate_to_pe(int *dest, int *ierr) noexcept
{
  *ierr = AMPI_Migrate_to_pe(*dest);
}

void ampi_set_migratable(int *mig, int *ierr) noexcept
{
  *ierr = AMPI_Set_migratable(*mig);
}

void ampi_register_pup(MPI_PupFn fn, void *data, int *idx, int *ierr) noexcept
{
  *ierr = AMPI_Register_pup(fn, data, idx);
}

void ampi_register_about_to_migrate(MPI_MigrateFn fn, int *ierr) noexcept
{
  *ierr = AMPI_Register_about_to_migrate(fn);
}

void ampi_register_just_migrated(MPI_MigrateFn fn, int *ierr) noexcept
{
  *ierr = AMPI_Register_just_migrated(fn);
}

void ampi_type_is_contiguous(int *datatype, int *flag, int *ierr) noexcept
{
  *ierr = AMPI_Type_is_contiguous(*datatype, flag);
}

void ampi_get_pup_data(int *idx, void *data, int *ierr) noexcept
{
  *ierr = AMPI_Get_pup_data(*idx, data);
}

void ampi_iget(MPI_Aint *orgdisp, int *orgcnt, int *orgtype, int *rank,
        MPI_Aint *targdisp, int *targcnt, int *targtype, int *win,
        int *request, int *ierr) noexcept
{
  *ierr = AMPI_Iget(*orgdisp, *orgcnt, *orgtype, *rank, *targdisp, *targcnt,
                    *targtype, *win, request);
}

void ampi_iget_wait(int *request, int *status, int *win, int *ierr) noexcept
{
  *ierr = AMPI_Iget_wait(request, (MPI_Status*)status, *win);
}

void ampi_iget_free(int *request, int *status, int *win, int *ierr) noexcept
{
  *ierr = AMPI_Iget_free(request, (MPI_Status*)status, *win);
}

void ampi_iget_data(void *data, int *status, int *ierr) noexcept
{
  *ierr = AMPI_Iget_data(data, *((MPI_Status*)status));
}

void ampi_alltoall_medium(void *data, int *sendcount, int *sendtype,
                          void *recvbuf, int *recvcount, int *recvtype,
                          int *comm, int *ierr) noexcept
{
  *ierr = AMPI_Alltoall_medium(data, *sendcount, *sendtype, recvbuf,
                               *recvcount, *recvtype, *comm);
}

void ampi_alltoall_long(void *data, int *sendcount, int *sendtype,
                        void *recvbuf, int *recvcount, int *recvtype,
                        int *comm, int *ierr) noexcept
{
  *ierr = AMPI_Alltoall_long(data, *sendcount, *sendtype, recvbuf,
                             *recvcount, *recvtype, *comm);
}

void ampi_yield(int *ierr) noexcept
{
  *ierr = AMPI_Yield();
}

void ampi_suspend(int *ierr) noexcept
{
  *ierr = AMPI_Suspend();
}

void ampi_resume(int *dest, int *comm, int *ierr) noexcept
{
  *ierr = AMPI_Resume(*dest, *comm);
}

void ampif_print(const char *str, int *len, int *ierr) noexcept
{
  char tmpStr[MPI_MAX_ERROR_STRING];
  ampif_str_f2c(tmpStr, str, *len);

  *ierr = AMPI_Print(tmpStr);
}

void ampi_trace_begin(int *ierr) noexcept
{
  *ierr = AMPI_Trace_begin();
}

void ampi_trace_end(int *ierr) noexcept
{
  *ierr = AMPI_Trace_end();
}


#if CMK_CUDA
void ampi_gpu_iinvoke_wr(int *to_call, int *request, int *ierr) noexcept {
  *ierr = AMPI_GPU_Iinvoke_wr((hapiWorkRequest *)to_call, request);
}

void ampi_gpu_invoke_wr(int *to_call, int *ierr) noexcept {
  *ierr = AMPI_GPU_Invoke_wr((hapiWorkRequest *)to_call);
}
#endif

/* Fortran2003 standard cmd line arg parsing functions:
 *    - command_argument_count() returns the number of arguments
 *      NOT including the program name.
 *    - get_command_argument() returns the i'th argument, where
 *      if 'i' is zero the program name is returned.
 */
void ampi_command_argument_count(int *count) noexcept {
  *count = AMPI_Get_argc()-1;
}

void ampi_get_command_argument(int *c, char *str, int *len, int *ierr) noexcept
{
  char **argv = AMPI_Get_argv();
  int nc = AMPI_Get_argc()-1;
  int arglen = strlen(argv[*c]);

  if (*c >= 0 && *c <= nc) {
    if (arglen <= *len) {
      memcpy(str, argv[*c], arglen);
      for (int j=arglen; j<*len; j++) str[j] = ' ';
      *ierr = 0;
    } else {
      memcpy(str, argv[*c], *len);
      *ierr = -1;
    }
  }
  else {
    memset(str, ' ', *len);
    *ierr = 1;
  }
}

} // FLINKAGE

CLINKAGE void romio_fortran_error_print(const char * str)
{
  fputs(str, stderr);
}
