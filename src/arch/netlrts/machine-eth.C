/** @file
 * UDP implementation of Converse NET version
 * @ingroup NET
 * contains only UDP specific code for:
 * - CmiMachineInit()
 * - CmiCommunicationInit()
 * - CmiNotifyIdle()
 * - DeliverViaNetwork()
 * - CommunicationServer()

  moved from machine.C by
  Gengbin Zheng, gzheng@uiuc.edu  4/22/2001
*/

/**
 * @addtogroup NET
 * @{
 */

/******************************************************************************
 *
 * CmiNotifyIdle()-- wait until a packet comes in
 *
 *****************************************************************************/

void LrtsStillIdle(void) {}
void LrtsNotifyIdle(void) {}
void LrtsBeginIdle(void) {}

/****************************************************************************
 *                                                                          
 * CheckSocketsReady
 *
 * Checks both sockets to see which are readable and which are writeable.
 * We check all these things at the same time since this can be done for
 * free with ``select.'' The result is stored in global variables, since
 * this is essentially global state information and several routines need it.
 *
 ***************************************************************************/

int CheckSocketsReady(int withDelayMs)
{
  int nreadable,dataWrite;
  CmiLock(Cmi_comm_var_mutex);
  dataWrite = writeableDgrams || writeableAcks;
  CmiUnlock(Cmi_comm_var_mutex);
  CMK_PIPE_DECL(withDelayMs);


#if CMK_USE_KQUEUE && 0
  // This implementation doesn't yet work, but potentially is much faster

  /* Only setup the CMK_PIPE structures the first time they are used. 
     This makes the kqueue implementation much faster.
  */
  static int first = 1;
  if(first){
    first = 0;
    CmiStdoutAdd(CMK_PIPE_SUB);
    if (Cmi_charmrun_fd!=-1) { CMK_PIPE_ADDREAD(Cmi_charmrun_fd); }
    else return 0; /* If there's no charmrun, none of this matters. */
    if (dataskt!=-1) {
      CMK_PIPE_ADDREAD(dataskt);
      CMK_PIPE_ADDWRITE(dataskt);
    }
  }

#else  
  CmiStdoutAdd(CMK_PIPE_SUB);  
  if (Cmi_charmrun_fd!=-1) { CMK_PIPE_ADDREAD(Cmi_charmrun_fd); }  
  else return 0; /* If there's no charmrun, none of this matters. */  
  if (dataskt!=-1) {  
    { CMK_PIPE_ADDREAD(dataskt); }  
    if (dataWrite)  
      CMK_PIPE_ADDWRITE(dataskt);  
  }  
#endif 

  nreadable=CMK_PIPE_CALL();
  ctrlskt_ready_read = 0;
  dataskt_ready_read = 0;
  dataskt_ready_write = 0;

  if (nreadable == 0) {
    MACHSTATE(1,"} CheckSocketsReady (nothing readable)")
    return nreadable;
  }
  if (nreadable==-1) {
    CMK_PIPE_CHECKERR();
    MACHSTATE(2,"} CheckSocketsReady (INTERRUPTED!)")
    return CheckSocketsReady(0);
  }
  
  CmiStdoutCheck(CMK_PIPE_SUB);
  if (Cmi_charmrun_fd!=-1) 
	  ctrlskt_ready_read = CMK_PIPE_CHECKREAD(Cmi_charmrun_fd);
  if (dataskt!=-1) {
	dataskt_ready_read = CMK_PIPE_CHECKREAD(dataskt);
	if (dataWrite)
		dataskt_ready_write = CMK_PIPE_CHECKWRITE(dataskt);
  }
  return nreadable;
}

/***********************************************************************
 * TransmitAckDatagram
 *
 * This function sends the ack datagram, after setting the window
 * array to show which of the datagrams in the current window have been
 * received. The sending side will then use this information to resend
 * packets, mark packets as received, etc. This system also prevents
 * multiple retransmissions/acks when acks are lost.
 ***********************************************************************/
void TransmitAckDatagram(OtherNode node)
{
  DgramAck ack; int i, seqno, slot; ExplicitDgram dg;
  int retval;
  
  DgramHeader *head;

  seqno = node->recv_next;
  MACHSTATE2(3,"  TransmitAckDgram [seq %d to 'pe' %d]",seqno,node->nodestart)
  DgramHeaderMake(&ack, DGRAM_ACKNOWLEDGE, Cmi_nodestartGlobal, Cmi_net_magic, seqno, 0);
  LOG(Cmi_clock, Cmi_nodestartGlobal, 'A', node->nodestart, seqno);
  for (i=0; i<Cmi_window_size; i++) {
    slot = seqno % Cmi_window_size;
    dg = node->recv_window[slot];
    ack.window[i] = (dg && (dg->seqno == seqno));
    seqno = ((seqno+1) & DGRAM_SEQNO_MASK);
  }
  memcpy(&ack.window[Cmi_window_size], &(node->send_ack_seqno), 
          sizeof(unsigned int));
  node->send_ack_seqno = ((node->send_ack_seqno + 1) & DGRAM_SEQNO_MASK);
  retval = (-1);
#if CMK_ERROR_CHECKING
  head = (DgramHeader *)(&ack);
  head->magic ^= computeCheckSum((unsigned char*)&ack, DGRAM_HEADER_SIZE + Cmi_window_size + sizeof(unsigned int));
#endif
  while(retval==(-1))
    retval = sendto(dataskt, (char *)&ack,
	 DGRAM_HEADER_SIZE + Cmi_window_size + sizeof(unsigned int), 0,
	 (struct sockaddr *)&(node->addr),
	 sizeof(struct sockaddr_in));
  node->stat_send_ack++;
}


/***********************************************************************
 * TransmitImplicitDgram
 * TransmitImplicitDgram1
 *
 * These functions do the actual work of sending a UDP datagram.
 ***********************************************************************/
void TransmitImplicitDgram(ImplicitDgram dg)
{
  char *data; DgramHeader *head; size_t len; DgramHeader temp;
  OtherNode dest;
  int retval;
  
  MACHSTATE3(3,"  TransmitImplicitDgram (%zu bytes) [seq %d to 'pe' %d]",
	     dg->datalen,dg->seqno,dg->dest->nodestart)
  len = dg->datalen;
  data = dg->dataptr;
  head = (DgramHeader *)(data - DGRAM_HEADER_SIZE);
  temp = *head;
  dest = dg->dest;
  DgramHeaderMake(head, dg->rank, dg->srcpe, Cmi_net_magic, dg->seqno, dg->broot);
#if CMK_ERROR_CHECKING
  head->magic ^= computeCheckSum((unsigned char*)head, len + DGRAM_HEADER_SIZE);
#endif
  LOG(Cmi_clock, Cmi_nodestartGlobal, 'T', dest->nodestart, dg->seqno);
  retval = (-1);
  while(retval==(-1))
    retval = sendto(dataskt, (char *)head, len + DGRAM_HEADER_SIZE, 0,
	      (struct sockaddr *)&(dest->addr), sizeof(struct sockaddr_in));
  *head = temp;
  dest->stat_send_pkt++;
}

void TransmitImplicitDgram1(ImplicitDgram dg)
{
  char *data; DgramHeader *head; size_t len; DgramHeader temp;
  OtherNode dest;
  int retval;

  MACHSTATE3(4,"  RETransmitImplicitDgram (%zu bytes) [seq %d to 'pe' %d]",
	     dg->datalen,dg->seqno,dg->dest->nodestart)
  len = dg->datalen;
  data = dg->dataptr;
  head = (DgramHeader *)(data - DGRAM_HEADER_SIZE);
  temp = *head;
  dest = dg->dest;
  DgramHeaderMake(head, dg->rank, dg->srcpe, Cmi_net_magic, dg->seqno, dg->broot);
#if CMK_ERROR_CHECKING
  head->magic ^= computeCheckSum((unsigned char *)head, len + DGRAM_HEADER_SIZE);
#endif
  LOG(Cmi_clock, Cmi_nodestartGlobal, 'P', dest->nodestart, dg->seqno);
  retval = (-1);
  while (retval == (-1))
    retval = sendto(dataskt, (char *)head, len + DGRAM_HEADER_SIZE, 0,
	      (struct sockaddr *)&(dest->addr), sizeof(struct sockaddr_in));
  *head = temp;
  dest->stat_resend_pkt++;
}


/***********************************************************************
 * TransmitAcknowledgement
 *
 * This function sends the ack datagrams, after checking to see if the 
 * Recv Window is atleast half-full. After that, if the Recv window size 
 * is 0, then the count of un-acked datagrams, and the time at which
 * the ack should be sent is reset.
 ***********************************************************************/
int TransmitAcknowledgement(void)
{
  int skip; static int nextnode=0; OtherNode node;
  for (skip=0; skip<CmiNumNodesGlobal(); skip++) {
    node = nodes+nextnode;
    nextnode = (nextnode + 1) % CmiNumNodesGlobal();
    if (node->recv_ack_cnt) {
      if ((node->recv_ack_cnt > Cmi_half_window) ||
	  (Cmi_clock >= node->recv_ack_time)) {
	TransmitAckDatagram(node);
	if (node->recv_winsz) {
	  node->recv_ack_cnt  = 1;
	  node->recv_ack_time = Cmi_clock + Cmi_ack_delay;
	} else {
	  node->recv_ack_cnt  = 0;
	  node->recv_ack_time = 0.0;
	}
	return 1;
      }
    }
  }
  return 0;
}


/***********************************************************************
 * TransmitDatagram()
 *
 * This function fills up the Send Window with the contents of the
 * Send Queue. It also sets the node->send_primer variable, which
 * indicates when a retransmission will be attempted.
 ***********************************************************************/
int TransmitDatagram(void)
{
  ImplicitDgram dg; OtherNode node;
  static int nextnode=0; int skip, count, slot;
  unsigned int seqno;
  
  for (skip=0; skip<CmiNumNodesGlobal(); skip++) {
    node = nodes+nextnode;
    nextnode = (nextnode + 1) % CmiNumNodesGlobal();
    CmiLock(node->send_queue_lock);
    dg = node->send_queue_h;
    if (dg) {
      seqno = dg->seqno;
      slot = seqno % Cmi_window_size;
      if (node->send_window[slot] == 0) {
	node->send_queue_h = dg->next;
	node->send_window[slot] = dg;
	CmiUnlock(node->send_queue_lock);
	TransmitImplicitDgram(dg); /*Actual transmisson happens here*/
	CmiLock(node->send_queue_lock);
	if (seqno == ((node->send_last+1)&DGRAM_SEQNO_MASK))
	  node->send_last = seqno;
	node->send_primer = Cmi_clock + Cmi_delay_retransmit;
	CmiUnlock(node->send_queue_lock);
	return 1;
      }
    }
    if (Cmi_clock > node->send_primer) {
      slot = (node->send_last % Cmi_window_size);
      for (count=0; count<Cmi_window_size; count++) {
	dg = node->send_window[slot];
	if (dg) {
          break;
        }
	slot = ((slot+Cmi_window_size-1) % Cmi_window_size);
      }
      if (dg) {
	CmiUnlock(node->send_queue_lock);
	TransmitImplicitDgram1(node->send_window[slot]); /*Actual transmisson happens here*/
	CmiLock(node->send_queue_lock);
	node->send_primer = Cmi_clock + Cmi_delay_retransmit;
	CmiUnlock(node->send_queue_lock);
	return 1;
      }
    }
    CmiUnlock(node->send_queue_lock);
  }
  return 0;
}

/***********************************************************************
 * EnqueOutgoingDgram()
 *
 * This function enqueues the datagrams onto the Send queue of the
 * sender, after setting appropriate data values into each of the
 * datagrams. 
 ***********************************************************************/
void EnqueueOutgoingDgram
        (OutgoingMsg ogm, char *ptr, size_t len, OtherNode node, int rank, int broot)
{
  int seqno, dst, src; ImplicitDgram dg;
  src = ogm->src;
  dst = ogm->dst;
  seqno = node->send_next;
  node->send_next = ((seqno+1)&DGRAM_SEQNO_MASK);
  MallocImplicitDgram(dg);
  dg->dest = node;
  dg->srcpe = src;
  dg->rank = rank;
  dg->seqno = seqno;
  dg->broot = broot;
  dg->dataptr = ptr;
  dg->datalen = len;
  dg->ogm = ogm;
  ogm->refcount++;
  dg->next = 0;
  if (node->send_queue_h == 0) {
    node->send_queue_h = dg;
    node->send_queue_t = dg;
  } else {
    node->send_queue_t->next = dg;
    node->send_queue_t = dg;
  }
}


/***********************************************************************
 * DeliverViaNetwork()
 *
 * This function is responsible for all non-local transmission. This
 * function takes the outgoing messages, splits it into datagrams and
 * enqueues them into the Send Queue.
 ***********************************************************************/
void DeliverViaNetwork(OutgoingMsg ogm, OtherNode node, int rank, unsigned int broot, int copy)
{
  size_t size; char *data;
  OtherNode myNode = nodes+CmiMyNodeGlobal();

  MACHSTATE2(3,"DeliverViaNetwork %d-byte message to pe %d",
	     ogm->size,node->nodestart+rank);
  size = ogm->size - DGRAM_HEADER_SIZE;
  data = ogm->data + DGRAM_HEADER_SIZE;

  CmiLock(node->send_queue_lock);
  while (size > Cmi_dgram_max_data) {
    EnqueueOutgoingDgram(ogm, data, Cmi_dgram_max_data, node, rank, broot);
    data += Cmi_dgram_max_data;
    size -= Cmi_dgram_max_data;
  }
  EnqueueOutgoingDgram(ogm, data, size, node, rank, broot);
  CmiUnlock(node->send_queue_lock);

  myNode->sent_msgs++;
  myNode->sent_bytes += ogm->size;
  /*Try to immediately send the packets off*/
  CmiLock(Cmi_comm_var_mutex);
  writeableDgrams=1;
  CmiUnlock(Cmi_comm_var_mutex);

}

/***********************************************************************
 * AssembleDatagram()
 *
 * This function does the actual assembly of datagrams into a
 * message. node->asm_msg holds the current message being
 * assembled. Once the message assemble is complete (known by checking
 * if the total number of datagrams is equal to the number of datagrams
 * constituting the assembled message), the message is pushed into the
 * Producer-Consumer queue
 ***********************************************************************/
void AssembleDatagram(OtherNode node, ExplicitDgram dg)
{
  int i;
  size_t size; char *msg;
  OtherNode myNode = nodes+CmiMyNodeGlobal();
  
  MACHSTATE3(2,"  AssembleDatagram [seq %d from 'pe' %d, packet len %d]",
  	dg->seqno,node->nodestart,dg->len)
  LOG(Cmi_clock, Cmi_nodestartGlobal, 'X', dg->srcpe, dg->seqno);
  msg = node->asm_msg;
  if (msg == 0) {
    size = CMI_MSG_SIZE(dg->data);
    MACHSTATE3(4,"  Assemble new datagram seq %d from 'pe' %d, len %d",
    	dg->seqno,node->nodestart,size)
    msg = (char *)CmiAlloc(size);
    if (!msg)
      fprintf(stderr, "%d: Out of mem\n", Lrts_myNode);
    if (size < dg->len) KillEveryoneCode(4559312);
#if CMK_CHARMDEBUG
    setMemoryTypeMessage(msg);
#endif
    memcpy(msg, (char*)(dg->data), dg->len);
    node->asm_rank = dg->rank;
    node->asm_total = size;
    node->asm_fill = dg->len;
    node->asm_msg = msg;
  } else {
    size = dg->len - DGRAM_HEADER_SIZE;
    memcpy(msg + node->asm_fill, ((char*)(dg->data))+DGRAM_HEADER_SIZE, size);
    node->asm_fill += size;
  }
  MACHSTATE3(2,"  AssembleDatagram: now have %d of %d bytes from %d",
  	node->asm_fill, node->asm_total, node->nodestart)
  if (node->asm_fill > node->asm_total) {
      fprintf(stderr, "\n\n\t\tLength mismatch!!\n\n");
      fflush(stderr);
      MACHSTATE4(5,"Length mismatch seq %d, from 'pe' %d, fill %d, total %d\n", dg->seqno,node->nodestart,node->asm_fill,node->asm_total)
      KillEveryoneCode(4559313);
  }
  if (node->asm_fill == node->asm_total) {
	//common core code  will handle where to send the messages
    handleOneRecvedMsg(node->asm_total, msg);
    node->asm_msg = 0;
    myNode->recd_msgs++;
    myNode->recd_bytes += node->asm_total;
  }
  FreeExplicitDgram(dg);
}


/***********************************************************************
 * AssembleReceivedDatagrams()
 *
 * This function assembles the datagrams received so far, into a
 * single message. This also results in part of the Receive Window being 
 * freed.
 ***********************************************************************/
void AssembleReceivedDatagrams(OtherNode node)
{
  unsigned int next, slot; ExplicitDgram dg;
  next = node->recv_next;
  while (1) {
    slot = (next % Cmi_window_size);
    dg = node->recv_window[slot];
    if (dg == 0) break;
    AssembleDatagram(node, dg);
    node->recv_window[slot] = 0;
    node->recv_winsz--;
    next = ((next + 1) & DGRAM_SEQNO_MASK);
  }
  node->recv_next = next;
}




/************************************************************************
 * IntegrateMessageDatagram()
 *
 * This function integrates the received datagrams. It first
 * increments the count of un-acked datagrams. (This is to aid the
 * heuristic that an ack should be sent when the Receive window is half
 * full). If the current datagram is the first missing packet, then this 
 * means that the datagram that was missing in the incomplete sequence
 * of datagrams so far, has arrived, and hence the datagrams can be
 * assembled. 
 ************************************************************************/

void IntegrateMessageDatagram(ExplicitDgram dg)
{
  int seqno;
  unsigned int slot; OtherNode node;

  LOG(Cmi_clock, Cmi_nodestartGlobal, 'M', dg->srcpe, dg->seqno);
  MACHSTATE2(2,"  IntegrateMessageDatagram [seq %d from pe %d]", dg->seqno,dg->srcpe)

  node = nodes_by_pe[dg->srcpe];
  node->stat_recv_pkt++;
  seqno = dg->seqno;
  writeableAcks=1;
  node->recv_ack_cnt++;
  if (node->recv_ack_time == 0.0)
    node->recv_ack_time = Cmi_clock + Cmi_ack_delay;
  if (((seqno - node->recv_next) & DGRAM_SEQNO_MASK) < Cmi_window_size) {
    slot = (seqno % Cmi_window_size);
    if (node->recv_window[slot] == 0) {
      node->recv_window[slot] = dg;
      node->recv_winsz++;
      if (seqno == node->recv_next)
	AssembleReceivedDatagrams(node);
      if (seqno > node->recv_expect)
	node->recv_ack_time = 0.0;
      if (seqno >= node->recv_expect)
	node->recv_expect = ((seqno+1)&DGRAM_SEQNO_MASK);
      LOG(Cmi_clock, Cmi_nodestartGlobal, 'Y', node->recv_next, dg->seqno);
      return;
    }
  }
  LOG(Cmi_clock, Cmi_nodestartGlobal, 'y', node->recv_next, dg->seqno);
  FreeExplicitDgram(dg);
}



/***********************************************************************
 * IntegrateAckDatagram()
 * 
 * This function is called on the message sending side, on receipt of
 * an ack for a message that it sent. Since messages and acks could be 
 * lost, our protocol works in such a way that acks for higher sequence
 * numbered packets act as implict acks for lower sequence numbered
 * packets, in case the acks for the lower sequence numbered packets
 * were lost.

 * Recall that the Send and Receive windows are circular queues, and the
 * sequence numbers of the packets (datagrams) are monotically
 * increasing. Hence it is important to know for which sequence number
 * the ack is for, and to correspodinly relate that to tha actual packet 
 * sitting in the Send window. Since every 20th packet occupies the same
 * slot in the windows, a number of sanity checks are required for our
 * protocol to work. 
 * 1. If the ack number (first missing packet sequence number) is less
 * than the last ack number received then this ack can be ignored. 

 * 2. The last ack number received must be set to the current ack
 * sequence number (This is done only if 1. is not true).

 * 3. Now the whole Send window is examined, in a kind of reverse
 * order. The check starts from a sequence number = 20 + the first
 * missing packet's sequence number. For each of these sequence numbers, 
 * the slot in the Send window is checked for existence of a datagram
 * that should have been sent. If there is no datagram, then the search
 * advances. If there is a datagram, then the sequence number of that is 
 * checked with the expected sequence number for the current iteration
 * (This is decremented in each iteration of the loop).

 * If the sequence numbers do not match, then checks are made (for
 * the unlikely scenarios where the current slot sequence number is 
 * equal to the first missing packet's sequence number, and where
 * somehow, packets which have greater sequence numbers than allowed for 
 * the current window)

 * If the sequence numbers DO match, then the flag 'rxing' is
 * checked. The semantics for this flag is that : If any packet with a
 * greater sequence number than the current packet (and hence in the
 * previous iteration of the for loop) has been acked, then the 'rxing'
 * flag is set to 1, to imply that all the packets of lower sequence
 * number, for which the ack->window[] element does not indicate that the 
 * packet has been received, must be retransmitted.
 * 
 ***********************************************************************/

void IntegrateAckDatagram(ExplicitDgram dg)
{
  OtherNode node; DgramAck *ack; ImplicitDgram idg;
  int i; unsigned int slot, rxing, dgseqno, seqno, ackseqno;
  int diff;
  unsigned int tmp;

  node = nodes_by_pe[dg->srcpe];
  ack = ((DgramAck*)(dg->data));
  memcpy(&ackseqno, &(ack->window[Cmi_window_size]), sizeof(unsigned int));
  dgseqno = dg->seqno;
  seqno = (dgseqno + Cmi_window_size) & DGRAM_SEQNO_MASK;
  slot = seqno % Cmi_window_size;
  rxing = 0;
  node->stat_recv_ack++;
  LOG(Cmi_clock, Cmi_nodestartGlobal, 'R', node->nodestart, dg->seqno);

  tmp = node->recv_ack_seqno;
  /* check that the ack being received is actually appropriate */
  if ( !((node->recv_ack_seqno >= 
	  ((DGRAM_SEQNO_MASK >> 1) + (DGRAM_SEQNO_MASK >> 2))) &&
	 (ackseqno < (DGRAM_SEQNO_MASK >> 1))) &&
       (ackseqno <= node->recv_ack_seqno))
    {
      FreeExplicitDgram(dg);
      return;
    } 
  /* higher ack so adjust */
  node->recv_ack_seqno = ackseqno;
  CmiLock(Cmi_comm_var_mutex);
  writeableDgrams=1; /* May have freed up some send slots */
  CmiUnlock(Cmi_comm_var_mutex);
  
  for (i=Cmi_window_size-1; i>=0; i--) {
    slot--; if (slot== ((unsigned int)-1)) slot+=Cmi_window_size;
    seqno = (seqno-1) & DGRAM_SEQNO_MASK;
    idg = node->send_window[slot];
    if (idg) {
      if (idg->seqno == seqno) {
	if (ack->window[i]) {
	  /* remove those that have been received and are within a window
	     of the first missing packet */
	  node->stat_ack_pkts++;
	  LOG(Cmi_clock, Cmi_nodestartGlobal, 'r', node->nodestart, seqno);
	  node->send_window[slot] = 0;
	  DiscardImplicitDgram(idg);
	  rxing = 1;
	} else if (rxing) {
	  node->send_window[slot] = 0;
	  idg->next = node->send_queue_h;
	  if (node->send_queue_h == 0) {
	    node->send_queue_t = idg;
	  }
	  node->send_queue_h = idg;
	}
      } else {
        diff = dgseqno >= idg->seqno ? 
	  ((dgseqno - idg->seqno) & DGRAM_SEQNO_MASK) :
	  ((dgseqno + (DGRAM_SEQNO_MASK - idg->seqno) + 1) & DGRAM_SEQNO_MASK);
	  
	if ((diff <= 0) || (diff > Cmi_window_size))
	{
	  continue;
	}

        /* if ack is really less than our packet seq (consider wrap around) */
        if (dgseqno < idg->seqno && (idg->seqno - dgseqno <= Cmi_window_size))
        {
          continue;
        }
        if (dgseqno == idg->seqno)
        {
	  continue;
        }
	node->stat_ack_pkts++;
	LOG(Cmi_clock, Cmi_nodestartGlobal, 'o', node->nodestart, idg->seqno);
	node->send_window[slot] = 0;
	DiscardImplicitDgram(idg);
      }
    }
  }
  FreeExplicitDgram(dg);  
}

void ReceiveDatagram(void)
{
  ExplicitDgram dg; int ok, magic;
  MACHLOCK_ASSERT(comm_flag,"ReceiveDatagram")
  MallocExplicitDgram(dg);
  ok = recv(dataskt,(char*)(dg->data),Cmi_max_dgram_size,0);
  /*ok = recvfrom(dataskt,(char*)(dg->data),Cmi_max_dgram_size,0, 0, 0);*/
  /* if (ok<0) { perror("recv"); KillEveryoneCode(37489437); } */
  if (ok < 0) {
    MACHSTATE1(4,"  recv dgram failed (errno=%d)",errno)
    FreeExplicitDgram(dg);
    if (errno == EINTR) return;  /* A SIGIO interrupted the receive */
    if (errno == EAGAIN) return; /* Just try again later */
#if !defined(_WIN32)
    if (errno == EWOULDBLOCK) return; /* No more messages on that socket. */
    if (errno == ECONNREFUSED) return;  /* A "Host unreachable" ICMP packet came in */
#endif
    CmiPrintf("ReceiveDatagram: recv: %s(%d)\n", strerror(errno), errno) ;
    KillEveryoneCode(37489437);
  }
  dg->len = ok;
#ifdef CMK_RANDOMLY_CORRUPT_MESSAGES
  /* randomly corrupt data and ack datagrams */
  randomCorrupt((char*)dg->data, dg->len);
#endif

  if (ok >= DGRAM_HEADER_SIZE) {
    DgramHeaderBreak(dg->data, dg->rank, dg->srcpe, magic, dg->seqno, dg->broot);
    MACHSTATE3(2,"  recv dgram [seq %d, for rank %d, from pe %d]",
	       dg->seqno,dg->rank,dg->srcpe)
#if CMK_ERROR_CHECKING
    if (computeCheckSum((unsigned char*)dg->data, dg->len) == 0)
#else
    if (magic == (Cmi_net_magic&DGRAM_MAGIC_MASK))
#endif
    {
      if (dg->rank == DGRAM_ACKNOWLEDGE)
	IntegrateAckDatagram(dg);
      else IntegrateMessageDatagram(dg);
    } else FreeExplicitDgram(dg);
  } else {
    MACHSTATE1(4,"  recv dgram failed (len=%d)",ok)
    FreeExplicitDgram(dg);
  }
}


/***********************************************************************
 * CommunicationServer()
 * 
 * This function does the scheduling of the tasks related to the
 * message sends and receives. It is called from the CmiGeneralSend()
 * function, and periodically from the CommunicationInterrupt() (in case 
 * of the single processor version), and from the comm_thread (for the
 * SMP version). Based on which of the data/control read/write sockets
 * are ready, the corresponding tasks are called
 *
 ***********************************************************************/
void CmiHandleImmediate(void);

static void CommunicationServerNet(int sleepTime, int where)
{
  unsigned int nTimes=0; /* Loop counter */
  LOG(GetClock(), Cmi_nodestartGlobal, 'I', 0, 0);
  MACHSTATE1(1,"CommunicationsServer(%d)", sleepTime)
#if !CMK_SHARED_VARS_UNAVAILABLE /*SMP mode: comm. lock is precious*/
  if (sleepTime!=0) {/*Sleep *without* holding the comm. lock*/
    MACHSTATE(1,"CommServer going to sleep (NO LOCK)");
    if (CheckSocketsReady(sleepTime)<=0) {
      MACHSTATE(1,"CommServer finished without anything happening.");
    }
  }
  sleepTime=0;
#endif
  CmiCommLock();
  inProgress[CmiMyRank()] += 1;
  /* in netpoll mode, only perform service to stdout */
  if (Cmi_netpoll && where == COMM_SERVER_FROM_INTERRUPT) {
    if (CmiStdoutNeedsService()) {CmiStdoutService();}
    CmiCommUnlock();
    inProgress[CmiMyRank()] -= 1;
    return;
  }
  CommunicationsClock();
  /*Don't sleep if a signal has stored messages for us*/
  if (sleepTime&&CmiState_hasMessage()) sleepTime=0;
  while (CheckSocketsReady(sleepTime)>0) {
    int again=0;
      MACHSTATE(2,"CheckSocketsReady returned true");
    sleepTime=0;
    if (ctrlskt_ready_read) {again=1;ctrl_getone();}
    if (dataskt_ready_read) {again=1;ReceiveDatagram();}
    if (dataskt_ready_write) {
      if (writeableAcks) 
        if (0!=(writeableAcks=TransmitAcknowledgement())) again=1;
      CmiLock(Cmi_comm_var_mutex);
      if (writeableDgrams) {
        writeableDgrams = 0;
        CmiUnlock(Cmi_comm_var_mutex);
        int temp;
        if (0!=(temp=TransmitDatagram())) {
          again=1;
          CmiLock(Cmi_comm_var_mutex);
          writeableDgrams = temp;
          CmiUnlock(Cmi_comm_var_mutex);
        }
      } else CmiUnlock(Cmi_comm_var_mutex);
    }
    if (CmiStdoutNeedsService()) {CmiStdoutService();}
    if (!again) break; /* Nothing more to do */
    if ((nTimes++ &16)==15) {
      /*We just grabbed a whole pile of packets-- try to retire a few*/
      CommunicationsClock();
    }
  }
  CmiCommUnlock();
  inProgress[CmiMyRank()] -= 1;

  /* when called by communication thread or in interrupt */
  if (where == COMM_SERVER_FROM_SMP || where == COMM_SERVER_FROM_INTERRUPT) {
#if CMK_IMMEDIATE_MSG
  CmiHandleImmediate();
#endif
#if CMK_PERSISTENT_COMM
  PumpPersistent();
#endif
  }

  MACHSTATE(1,"} CommunicationServerNet")
}

void CmiMachineInit(char **argv)
{
}

void CmiCommunicationInit(char **argv)
{
}

void MachineExit(void)
{
}


