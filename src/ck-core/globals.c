/***************************************************************************
 * RCS INFORMATION:
 *
 *	$RCSfile$
 *	$Author$	$Locker$		$State$
 *	$Revision$	$Date$
 *
 ***************************************************************************
 * DESCRIPTION:
 *
 ***************************************************************************
 * REVISION HISTORY:
 *
 * $Log$
 * Revision 2.9  1995-09-30 15:02:25  jyelon
 * Fixed a missing CpvInitialize.
 *
 * Revision 2.8  1995/09/20  15:41:27  gursoy
 * added new handler indexes
 *
 * Revision 2.7  1995/09/14  20:47:38  jyelon
 * Added +fifo +lifo +ififo +ilifo +bfifo +blifo command-line options.
 *
 * Revision 2.6  1995/09/07  05:26:38  gursoy
 * introduced new global variables used by HANDLE_INIT_MSG
 *
 * Revision 2.5  1995/09/06  21:48:50  jyelon
 * Eliminated 'CkProcess_BocMsg', using 'CkProcess_ForChareMsg' instead.
 *
 * Revision 2.4  1995/09/01  02:13:17  jyelon
 * VID_BLOCK, CHARE_BLOCK, BOC_BLOCK consolidated.
 *
 * Revision 2.3  1995/07/27  20:29:34  jyelon
 * Improvements to runtime system, general cleanup.
 *
 * Revision 2.2  1995/06/13  14:33:55  gursoy
 * *** empty log message ***
 *
 * Revision 1.7  1995/05/04  22:03:51  jyelon
 * *** empty log message ***
 *
 * Revision 1.6  1995/05/03  20:57:13  sanjeev
 * bug fixes for finding uninitialized modules
 *
 * Revision 1.5  1995/04/02  00:47:16  sanjeev
 * changes for separating Converse
 *
 * Revision 1.4  1995/03/12  17:08:13  sanjeev
 * changes for new msg macros
 *
 * Revision 1.3  1995/01/17  23:45:52  knauff
 * Added variables for the '++outputfile' option in the network version.
 *
 * Revision 1.2  1994/12/01  23:55:06  sanjeev
 * interop stuff
 *
 * Revision 1.1  1994/11/03  17:38:32  brunner
 * Initial revision
 *
 ***************************************************************************/
static char ident[] = "@(#)$Header$";
#include "chare.h"
#include "trans_defs.h"
#include <stdio.h>

/**********************************************************************/
/* These fields are needed by message macros. Any changes must be
reflected there. */
/**********************************************************************/

CpvDeclare(int, PAD_SIZE);
CpvDeclare(int, HEADER_SIZE);
CpvDeclare(int, _CK_Env_To_Usr);
CpvDeclare(int, _CK_Ldb_To_Usr);
CpvDeclare(int, _CK_Usr_To_Env);
CpvDeclare(int, _CK_Usr_To_Ldb);




/**********************************************************************/
/* Other global variables. */
/**********************************************************************/
CsvDeclare(int, TotalEps);
CpvDeclare(int, NumReadMsg);
CpvDeclare(int, MsgCount); 	/* for the initial, pre-loop phase.
				to count up all the messages
		 		being sent out to nodes 		*/
CpvDeclare(int, InsideDataInit);
CpvDeclare(int, mainChare_magic_number);
typedef struct chare_block *CHARE_BLOCK_;
CpvDeclare(CHARE_BLOCK_,  mainChareBlock);
CpvDeclare(CHARE_BLOCK_, currentChareBlock);
CpvDeclare(int, currentBocNum);
CpvDeclare(int, MainDataSize);  /* size of dataarea for main chare 	*/


CsvDeclare(FUNCTION_PTR*, ROCopyFromBufferTable);
CsvDeclare(FUNCTION_PTR*, ROCopyToBufferTable);
CsvDeclare(EP_STRUCT*, EpInfoTable);
CsvDeclare(MSG_STRUCT*, MsgToStructTable);
CsvDeclare(PSEUDO_STRUCT*, PseudoTable);
CsvDeclare(int*, ChareSizesTable);
CsvDeclare(FUNCTION_PTR*, ChareFnTable);
CsvDeclare(char**, ChareNamesTable);

CpvDeclare(int, msgs_processed);
CpvDeclare(int, msgs_created);

CpvDeclare(int, nodecharesCreated);
CpvDeclare(int, nodeforCharesCreated);
CpvDeclare(int, nodebocMsgsCreated);
CpvDeclare(int, nodecharesProcessed);
CpvDeclare(int, nodebocMsgsProcessed);
CpvDeclare(int, nodeforCharesProcessed);


CpvDeclare(int, PrintChareStat); 
CpvDeclare(int, PrintSummaryStat);
CpvDeclare(int, QueueingDefault);

CpvDeclare(int, RecdStatMsg);
CpvDeclare(int, RecdPerfMsg);

CpvDeclare(int, numHeapEntries);      /* heap of tme-dep calls   */
CpvDeclare(int, numCondChkArryElts);  /* arry hldng conditon check info */


CpvDeclare(int, _CK_13PackOffset);
CpvDeclare(int, _CK_13PackMsgCount);
CpvDeclare(int, _CK_13ChareEPCount);
CpvDeclare(int, _CK_13TotalMsgCount);

CsvDeclare(FUNCTION_PTR*,  _CK_9_GlobalFunctionTable);

CsvDeclare(int, MainChareLanguage);

/* Handlers for various message-types */
CsvDeclare(int, BUFFER_INCOMING_MSG_Index);
CsvDeclare(int, MAIN_HANDLE_INCOMING_MSG_Index);
CsvDeclare(int, HANDLE_INCOMING_MSG_Index);
CsvDeclare(int, HANDLE_INIT_MSG_Index);
CsvDeclare(int, CkProcIdx_ForChareMsg);
CsvDeclare(int, CkProcIdx_DynamicBocInitMsg);
CsvDeclare(int, CkProcIdx_NewChareMsg);
CsvDeclare(int, CkProcIdx_VidSendOverMsg);

/* System-defined chare numbers */
CsvDeclare(int, CkChare_ACC);
CsvDeclare(int, CkChare_MONO);

/* Entry points for Quiescence detection BOC 	*/
CsvDeclare(int, CkEp_QD_Init);
CsvDeclare(int, CkEp_QD_InsertQuiescenceList);
CsvDeclare(int, CkEp_QD_PhaseIBroadcast);
CsvDeclare(int, CkEp_QD_PhaseIMsg);
CsvDeclare(int, CkEp_QD_PhaseIIBroadcast);
CsvDeclare(int, CkEp_QD_PhaseIIMsg);

/* Entry points for Write Once Variables 	*/
CsvDeclare(int, CkEp_WOV_AddWOV);
CsvDeclare(int, CkEp_WOV_RcvAck);
CsvDeclare(int, CkEp_WOV_HostAddWOV);
CsvDeclare(int, CkEp_WOV_HostRcvAck);

/* Entry points for dynamic tables BOC    	*/
CsvDeclare(int, CkEp_Tbl_Unpack);

/* Entry points for accumulator BOC		*/
CsvDeclare(int, CkEp_ACC_CollectFromNode);
CsvDeclare(int, CkEp_ACC_LeafNodeCollect);
CsvDeclare(int, CkEp_ACC_InteriorNodeCollect);
CsvDeclare(int, CkEp_ACC_BranchInit);

/* Entry points for monotonic BOC		*/
CsvDeclare(int, CkEp_MONO_BranchInit);
CsvDeclare(int, CkEp_MONO_BranchUpdate);
CsvDeclare(int, CkEp_MONO_ChildrenUpdate);

/* These are the entry points necessary for the dynamic BOC creation. */
CsvDeclare(int, CkEp_DBOC_RegisterDynamicBocInitMsg);
CsvDeclare(int, CkEp_DBOC_OtherCreateBoc);
CsvDeclare(int, CkEp_DBOC_InitiateDynamicBocBroadcast);

/* These are the entry points for the statistics BOC */
CsvDeclare(int, CkEp_Stat_CollectNodes);
CsvDeclare(int, CkEp_Stat_Data);
CsvDeclare(int, CkEp_Stat_PerfCollectNodes);
CsvDeclare(int, CkEp_Stat_BroadcastExitMessage);
CsvDeclare(int, CkEp_Stat_ExitMessage);

/* Entry points for LoadBalancing BOC 		*/
CsvDeclare(int, CkEp_Ldb_NbrStatus);

CsvDeclare(int, NumSysBocEps);


/* Initialization phase count variables for synchronization */
CpvDeclare(int,CkInitCount);
CpvDeclare(int,CkCountArrived);


/* Buffer for the non-init messages received during the initialization phase */
CpvDeclare(void*, CkBuffQueue);


/* Initialization phase flag : 1 if in the initialization phase */
CpvDeclare(int, CkInitPhase);










void globalsModuleInit()
{
   CpvInitialize(int, PAD_SIZE);
   CpvInitialize(int, HEADER_SIZE);
   CpvInitialize(int, _CK_Env_To_Usr);
   CpvInitialize(int, _CK_Ldb_To_Usr);
   CpvInitialize(int, _CK_Usr_To_Env);
   CpvInitialize(int, _CK_Usr_To_Ldb);
   CpvInitialize(int, NumReadMsg);
   CpvInitialize(int, MsgCount); 
   CpvInitialize(int, InsideDataInit);
   CpvInitialize(int, mainChare_magic_number);
   CpvInitialize(CHARE_BLOCK_,  mainChareBlock);
   CpvInitialize(CHARE_BLOCK_, currentChareBlock);
   CpvInitialize(int, currentBocNum);
   CpvInitialize(int, MainDataSize); 
   CpvInitialize(int, msgs_processed);
   CpvInitialize(int, msgs_created);
   CpvInitialize(int, nodecharesCreated);
   CpvInitialize(int, nodeforCharesCreated);
   CpvInitialize(int, nodebocMsgsCreated);
   CpvInitialize(int, nodecharesProcessed);
   CpvInitialize(int, nodebocMsgsProcessed);
   CpvInitialize(int, nodeforCharesProcessed);
   CpvInitialize(int, PrintChareStat);
   CpvInitialize(int, PrintSummaryStat);
   CpvInitialize(int, QueueingDefault);
   CpvInitialize(int, RecdStatMsg);
   CpvInitialize(int, RecdPerfMsg);
   CpvInitialize(int, numHeapEntries);      
   CpvInitialize(int, numCondChkArryElts); 
   CpvInitialize(int, _CK_13PackOffset);
   CpvInitialize(int, _CK_13PackMsgCount);
   CpvInitialize(int, _CK_13ChareEPCount);
   CpvInitialize(int, _CK_13TotalMsgCount);
   CpvInitialize(int, CkInitPhase);
   CpvInitialize(int, CkInitCount);
   CpvInitialize(int, CkCountArrived);
   CpvInitialize(void*, CkBuffQueue); 

   CpvAccess(NumReadMsg)             = 0; 
   CpvAccess(InsideDataInit)         = 0;
   CpvAccess(currentBocNum)          = (NumSysBoc - 1); /* was set to  -1 */
   CpvAccess(nodecharesCreated)      = 0;
   CpvAccess(nodeforCharesCreated)   = 0;
   CpvAccess(nodebocMsgsCreated)     = 0;
   CpvAccess(nodecharesProcessed)    = 0;
   CpvAccess(nodebocMsgsProcessed)   = 0;
   CpvAccess(nodeforCharesProcessed) = 0;
   CpvAccess(PrintChareStat)         = 0;
   CpvAccess(PrintSummaryStat)       = 0;
   CpvAccess(numHeapEntries)         = 0;  
   CpvAccess(numCondChkArryElts)     = 0; 
   CpvAccess(CkInitPhase)            = 1;
   CpvAccess(CkInitCount)            = 0;
   CpvAccess(CkCountArrived)         = 0; 
   CpvAccess(CkBuffQueue)            = NULL;   

   if (CmiMyRank() == 0) CsvAccess(MainChareLanguage)  = -1;
}
