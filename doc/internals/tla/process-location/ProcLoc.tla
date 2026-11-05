------------------------------- MODULE ProcLoc -------------------------------
(***************************************************************************)
(* Location management with a process-level location table, as in charm   *)
(* PR #4123 (branch objid-pr3-loccache @ cc84660f1, 2026-11-03), and with   *)
(* Fix = TRUE the fix for the lost-message finding (life tags, section 3.5). *)
(* doc/internals/process-location-cache.md, src/ck-core/cklocnode.{h,C},   *)
(* cklocation.{h,C}, ckarray.C.                                            *)
(*                                                                         *)
(* Scope: one element of a packed-kind array (index <-> id fixed, so       *)
(* lookupID always succeeds and recoverIndex never fails; no rebinding, no  *)
(* retired ids).  The element is created during the run on any PE, then    *)
(* migrates, within and across processes; with MaxDel > 0 it can also die  *)
(* (ckDestroy) and be created again under the same id, at epoch 0.  Point-to-point messages with    *)
(* the default "buffer" if-not-there policy, small or large (a large one is *)
(* held where it cannot be delivered instead of being sent to the home).   *)
(* Unordered delivery (reconverse promises no order).  Not modeled:        *)
(* demand creation, CMK_GLOBAL_LOCATION_UPDATE, zero-copy, GPU, the      *)
(* hashed kind, more than 64 PEs per process (waiter-bit fold).            *)
(*                                                                         *)
(* Concurrency: the PEs of a process share one table entry (value word with*)
(* pe and epoch; waiter word) and run concurrently.  A handler is atomic   *)
(* only against the other handlers of its own PE.  The one handler whose   *)
(* table accesses can interleave with another PE's in a way that matters   *)
(* is requestLocation's register-then-reread, so it is split into two      *)
(* steps with a per-PE program counter (pc); while pc[p] is not idle, no   *)
(* other handler runs on p.  Store/update and the take of the waiter bits  *)
(* are modeled as one step (the writer's CAS and exchange are two atomics; *)
(* a register between them only earns a spare wake).  Emigration's record  *)
(* then send is one step: nobody can observe the gap, since the migration  *)
(* message is received in a later step anyway.                             *)
(*                                                                         *)
(* Source mapping is given at each action.                                 *)
(***************************************************************************)
EXTENDS Integers, FiniteSets, TLC

CONSTANTS
    Procs,      \* set of process numbers, e.g. {0,1}
    PPN,        \* PEs per process, e.g. 2 (PE p is in process p \div PPN)
    Home,       \* the element's home PE (homePe(id))
    MaxMig,     \* bound on the number of migrations
    MsgIds,     \* identities of application messages, e.g. {1,2}
    BCap,       \* saturation cap of the per-message forward counter (probes)
    MaxDel,     \* bound on deaths (0: the element never dies)
    MaxCreate,  \* bound on creations (MaxDel + 1 lets every death be followed by a re-creation)
    Fix,        \* FALSE: the code at cc84660f1.  TRUE: the fix (README): own-write
                \* protection, the by-id drop in the home's process, and (with Rec)
                \* life tags and the home's record of lives (CkLocMgr::homeLives,
                \* deadLives, homeReinstall).
    Rec,        \* with Fix: TRUE = the home keeps its record of lives (2f144b0e7);
                \* FALSE = no record, no life tags: only the compare-and-clear on a
                \* notice plus the two free rules (the proposal of the analysis).
    DeadSet,    \* with Fix and not Rec: the home keeps only the set of dead lives,
                \* drops their updates, and clears its entry unconditionally on a
                \* notice (option C'' of the analysis; needs Sync for liveness).
    AppSync,    \* TRUE: Kale's weaker condition: the program deletes the element only
                \* after every application message to it has been processed (none in
                \* flight or held); runtime messages are not constrained.
    Sync,       \* TRUE: a re-creation happens only after every runtime message of
                \* the earlier life has been delivered (the contract: a global
                \* synchronization between the deletion and the re-insertion).
    Mutation    \* 0: the code.  1..4: deliberate protocol errors (see README)

PEs == {P * PPN + r : P \in Procs, r \in 0..PPN-1}
Proc(p) == p \div PPN
Ranks(P) == {p \in PEs : Proc(p) = P}

ASSUME Home \in PEs /\ MaxMig \in Nat /\ BCap \in Nat \ {0} /\ Mutation \in 0..4
ASSUME MaxDel \in Nat /\ MaxCreate \in Nat \ {0} /\ Fix \in BOOLEAN /\ Rec \in BOOLEAN /\ Sync \in BOOLEAN /\ DeadSet \in BOOLEAN /\ AppSync \in BOOLEAN
UseRec == Fix /\ Rec
UseDead == Fix /\ ~Rec /\ DeadSet

None == -1
NotCreated == -2
HCap == 2      \* only "hops > 1" is ever tested, so hops saturates at 2

VARIABLES
    holder,     \* PE running the element; None while an immigrate is in flight;
                \* NotCreated before creation
    epoch,      \* the element's true epoch (emigrations in its current life)
    migs,       \* migrations started so far
    creates,    \* creations so far
    dels,       \* deaths so far
    tab,        \* tab[P] = [pe, ep, w]: the process table's entry for the element
                \* (value word: location pe and epoch ep; waiter word: set of PEs w)
    pc,         \* pc[p]: idle, or the state of a split requestLocation on PE p
    net,        \* set of messages in flight (unordered)
    buf,        \* buf[p]: app messages held in CkArray::bufferedIDMsgs on p
    pendReq,    \* PEs whose request the home holds (bufferedLocationRequests)
    sent,       \* app message ids injected so far
    msrc,       \* msrc[m]: originating PE (array_getSrcPe)
    mlarge,     \* mlarge[m]: size >= _messageBufferingThreshold
    dcount,     \* dcount[m]: number of times m was delivered to the element
    hl,         \* Fix: the home's record of lives: {[pe, ep, life]}, the latest location
                \* the home saw for each life not known dead (CkLocMgr::homeLives)
    dead        \* Fix: lives whose death the home has processed (deadLives)

vars == <<holder, epoch, migs, creates, dels, tab, pc, net, buf, pendReq, sent, msrc, mlarge, dcount, hl, dead>>

Min(a, b) == IF a < b THEN a ELSE b

AppMsg(to, m, h, b)   == [type |-> "app", to |-> to, mid |-> m, hops |-> h, b |-> b]
ImmMsg(to, e)         == [type |-> "imm", to |-> to, ep |-> e]  \* life: the current one
\* idx = TRUE: CkLocMgr::updateLocation(idx, e) (informHome, replies by index);
\* idx = FALSE: CkLocCache::updateLocation(e) (reply to a request by id, repair).
UpdMsgL(to, q, e, idx, l) == [type |-> "upd", to |-> to, pe |-> q, ep |-> e, idx |-> idx, life |-> l]
\* Every home update and death notice carries the life it was sent in (the life tag
\* of the fix; a creation count serves as the tag here, compared for equality only).
\* Without Fix the protocol ignores it; the model still needs it so that two notices
\* from different lives are two messages, not one (identical messages merge in the
\* set `net`).  Replies and repairs built from the table carry 0, as in the code.
UpdMsg(to, q, e, idx) == UpdMsgL(to, q, e, idx, creates)
ReqMsg(tell)          == [type |-> "req", to |-> Home, tell |-> tell]
WakeMsg(to)           == [type |-> "wake", to |-> to]
\* CkLocMgr::reclaimRemote(idx, deletedOnPe, epoch) to the home.
DeadMsg(q, e)         == [type |-> "dead", to |-> Home, pe |-> q, ep |-> e, life |-> creates]
BufRec(m, h, b)       == [mid |-> m, hops |-> h, b |-> b]

Idle == [kind |-> "idle"]
\* After registerWaiter in requestLocation(id), before the re-read.  kind "hold":
\* bufferForLocation / handleUnknownByID holds rec afterwards unless the re-read
\* finds the location; kind "wake": CkLocCache::wake re-registering.
HoldPC(rec, first) == [kind |-> "hold", rec |-> rec, first |-> first]
WakePC(first)      == [kind |-> "wake", first |-> first]

\* own: written by a PE of the process about itself (W1, W2, W3), not from a
\* received update (W4); the value word's byMessage mark, inverted.
Entry == [pe : PEs \cup {None}, ep : {None} \cup 0..MaxMig, w : SUBSET PEs, own : BOOLEAN]
LifeRec == [pe : PEs, ep : 0..MaxMig, life : 1..MaxCreate]
Recs  == [mid : MsgIds, hops : 0..HCap, b : 0..BCap]

MsgUniverse ==
       {AppMsg(t, m, h, b) : t \in PEs, m \in MsgIds, h \in 0..HCap, b \in 0..BCap}
  \cup {ImmMsg(t, e) : t \in PEs, e \in 1..MaxMig}
  \cup {[type |-> "upd", to |-> t, pe |-> q, ep |-> e, idx |-> i, life |-> l] :
          t \in PEs, q \in PEs, e \in 0..MaxMig, i \in BOOLEAN, l \in 0..MaxCreate}
  \cup {ReqMsg(p) : p \in PEs}
  \cup {WakeMsg(p) : p \in PEs}
  \cup {[type |-> "dead", to |-> Home, pe |-> q, ep |-> e, life |-> l] :
          q \in PEs, e \in 0..MaxMig, l \in 0..MaxCreate}

TypeOK ==
    /\ holder \in PEs \cup {None, NotCreated}
    /\ epoch \in 0..MaxMig /\ migs \in 0..MaxMig
    /\ creates \in 0..MaxCreate /\ dels \in 0..MaxDel
    /\ tab \in [Procs -> Entry]
    /\ \A P \in Procs : tab[P].w \subseteq Ranks(P)
    /\ pc \in [PEs -> {Idle} \cup {HoldPC(r, f) : r \in Recs, f \in BOOLEAN}
                              \cup {WakePC(f) : f \in BOOLEAN}]
    /\ net \subseteq MsgUniverse
    /\ buf \in [PEs -> SUBSET Recs]
    /\ pendReq \subseteq PEs
    /\ sent \subseteq MsgIds
    /\ dcount \in [MsgIds -> 0..2]
    /\ hl \subseteq LifeRec /\ dead \subseteq 1..MaxCreate

NoEntry == [pe |-> None, ep |-> None, w |-> {}, own |-> FALSE]

Init_ ==
    /\ holder = NotCreated
    /\ epoch = 0
    /\ migs = 0
    /\ creates = 0
    /\ dels = 0
    /\ tab = [P \in Procs |-> NoEntry]
    /\ pc = [p \in PEs |-> Idle]
    /\ net = {}
    /\ buf = [p \in PEs |-> {}]
    /\ pendReq = {}
    /\ sent = {}
    /\ msrc = [m \in MsgIds |-> None]
    /\ mlarge = [m \in MsgIds |-> FALSE]
    /\ dcount = [m \in MsgIds |-> 0]
    /\ hl = {} /\ dead = {}

-----------------------------------------------------------------------------
(* Table operations (cklocnode.C LocTable).  Each is one atomic step.      *)

\* Wake messages for the waiter bits taken by a write on PE p (CkLocCache::
\* wakeRanks: the writer's own rank is notified directly, not by message).
Wakes(bits, p) == IF Mutation = 3 THEN {} ELSE {WakeMsg(q) : q \in bits \ {p}}

\* W1/W3: LocTable::store, unconditional; takes the waiter bits.
Store(P, q, e) == [pe |-> q, ep |-> e, w |-> {}, own |-> TRUE]
\* W4: the written value, marked byMessage.
Recv4(q, e) == [pe |-> q, ep |-> e, w |-> {}, own |-> FALSE]

\* What a PE of process P wrote about itself (W1, W2, W3): never replaced by a
\* received update nor cleared by the home (Fix).
OwnWrite(P, T) == Fix /\ T.pe # None /\ Proc(T.pe) = P /\ T.own

\* W4: LocTable::update for a received update: written only if the entry has no
\* location or e is newer, and (Fix) the entry is not the process's own write.
Written(P, e) == (tab[P].pe = None \/ e > tab[P].ep) /\ ~OwnWrite(P, tab[P])

\* Cleared: the entry without its location (W5; key and waiter bits kept).
Cleared(T) == [T EXCEPT !.pe = None, !.ep = None, !.own = FALSE]

\* Re-send every held message of p toward q: CkArray::sendBufferedMsgs(id, pe)
\* through the location listener, sendToPe(msg, q, CkDeliver_queue).
Flush(p, q) == {AppMsg(q, r.mid, r.hops, r.b) : r \in buf[p]}

-----------------------------------------------------------------------------
(* Fix: the home's record of lives (cklocation.C homeNoteLocation, homeNoteDeath, *)
(* homeReinstall).  The table carries no life; the home keeps the latest location *)
(* it saw for each life not known dead, and the set of dead lives.                *)
HomeRec(q, e, l) == [pe |-> q, ep |-> e, life |-> l]
Upsert(S, x) == IF \E y \in S : y.life = x.life /\ y.ep >= x.ep THEN S
                ELSE {y \in S : y.life # x.life} \cup {x}
\* An update older than the home's record for its life: dropped like a dead life's
\* (it would revive a cleared entry with a state the life has left).
Stale(x) == \E y \in hl : y.life = x.life /\ y.ep > x.ep

\* homeReinstall -> updateLocation(idx, x): W4 on the empty entry (written), wakes,
\* flush of the home's held messages toward x.pe, answers to the held requests.
Install(x, rest) ==
    LET P == Proc(Home) IN  \* the entry is empty: W4 writes it
    /\ tab' = [tab EXCEPT ![P] = Recv4(x.pe, x.ep)]
    /\ buf' = [buf EXCEPT ![Home] = {}]
    /\ pendReq' = {}
    /\ net' = rest \cup Wakes(tab[P].w, Home) \cup Flush(Home, x.pe)
              \cup {UpdMsgL(q, x.pe, x.ep, TRUE, x.life) : q \in pendReq \ {Home}}

\* After a clear at the home: T1 is the home process's entry after it, hl1 the
\* record after it.  Reinstall when the entry is empty and a life is recorded.
AfterClear(T1, hl1, rest) ==
    IF Fix /\ T1.pe = None /\ hl1 # {}
      THEN \E x \in hl1 : Install(x, rest)
      ELSE /\ tab' = [tab EXCEPT ![Proc(Home)] = T1]
           /\ net' = rest
           /\ UNCHANGED <<buf, pendReq>>

-----------------------------------------------------------------------------
(* CkLocCache::requestLocation(id), first half: registerWaiter.  The caller *)
(* (bufferForLocation / handleUnknownByID) has found no location and holds *)
(* no message for the id yet ("fresh"); otherwise it just pushes.          *)
Hold(p, rec) ==
    LET P == Proc(p) IN
    IF buf[p] = {} THEN
       /\ IF Mutation = 2
            THEN /\ pc' = [pc EXCEPT ![p] = HoldPC(rec, tab[P].w = {})]
                 /\ UNCHANGED tab
            ELSE /\ tab' = [tab EXCEPT ![P].w = @ \cup {p}]
                 /\ pc' = [pc EXCEPT ![p] = HoldPC(rec, tab[P].w = {})]
       /\ UNCHANGED buf
    ELSE
       /\ buf' = [buf EXCEPT ![p] = @ \cup {rec}]
       /\ UNCHANGED <<tab, pc>>

(* CkLocCache::requestLocation(id), second half: re-read the value.  Found:  *)
(* drop the own bit and send (or flush).  Not found: the first registrant   *)
(* sends the request to the home unless the home is in this process; then   *)
(* hold.                                                                    *)
Reread(p) ==
    /\ pc[p].kind # "idle"
    /\ LET P == Proc(p)
           e == tab[P]
           first == pc[p].first
           sendReq == first /\ Proc(Home) # P /\ Mutation # 4
       IN
       IF e.pe # None THEN
          /\ tab' = [tab EXCEPT ![P].w = @ \ {p}]
          /\ IF pc[p].kind = "hold"
               THEN /\ net' = net \cup {AppMsg(e.pe, pc[p].rec.mid, pc[p].rec.hops, pc[p].rec.b)}
                    /\ UNCHANGED buf
               ELSE /\ net' = net \cup Flush(p, e.pe)
                    /\ buf' = [buf EXCEPT ![p] = {}]
       ELSE
          /\ net' = IF sendReq THEN net \cup {ReqMsg(p)} ELSE net
          /\ buf' = IF pc[p].kind = "hold" THEN [buf EXCEPT ![p] = @ \cup {pc[p].rec}] ELSE buf
          /\ UNCHANGED tab
    /\ pc' = [pc EXCEPT ![p] = Idle]
    /\ UNCHANGED <<holder, epoch, migs, creates, dels, pendReq, sent, msrc, mlarge, dcount, hl, dead>>

-----------------------------------------------------------------------------
(* ckarray.C CkArray::sendMsg on the originating PE s.  Packed kind: the id *)
(* is always known; whichPe reads the process table.                        *)
Send(m, s, large) ==
    /\ m \notin sent /\ pc[s].kind = "idle"
    /\ sent' = sent \cup {m}
    /\ msrc' = [msrc EXCEPT ![m] = s]
    /\ mlarge' = [mlarge EXCEPT ![m] = large]
    /\ LET d == tab[Proc(s)].pe IN
       IF d # None
         THEN /\ net' = net \cup {AppMsg(d, m, 0, 0)}                      \* sendToPe(dest)
              /\ UNCHANGED <<buf, tab, pc>>
         ELSE IF ~large /\ s # Home                                        \* handleUnknown: small, not home
           THEN /\ net' = net \cup {AppMsg(Home, m, 0, 0)}
                /\ UNCHANGED <<buf, tab, pc>>
           ELSE /\ Hold(s, BufRec(m, 0, 0))                                \* bufferForLocation
                /\ UNCHANGED net
    /\ UNCHANGED <<holder, epoch, migs, creates, dels, pendReq, dcount, hl, dead>>

(* ckarray.C CkArray::recvMsg (hops++): deliverToElement + CkLocMgr::multiHop,  *)
(* or forward by the process table, or handleUnknownByID.                      *)
RecvApp(r) ==
    /\ r \in net /\ r.type = "app"
    /\ LET p  == r.to
           P  == Proc(p)
           m  == r.mid
           h1 == Min(r.hops + 1, HCap)
           b1 == Min(r.b + 1, BCap)
           rest == net \ {r}
           src == msrc[m]
       IN
       /\ pc[p].kind = "idle"
       /\ IF holder = p THEN
             \* deliverToElement; multiHop repairs a sender in another process:
             \* cache->requestLocation(id, srcPe) run here, from this table's entry.
             \* With the entry empty (a death-notice window, README) the request is
             \* held on this PE, which is not the home, and never answered: no repair.
             /\ dcount' = [dcount EXCEPT ![m] = Min(@ + 1, 2)]
             /\ net' = IF h1 > 1 /\ src # p /\ Proc(src) # P /\ tab[P].pe # None
                         THEN rest \cup {[type |-> "upd", to |-> src, pe |-> tab[P].pe, ep |-> tab[P].ep,
                                          idx |-> FALSE, life |-> 0]}
                         ELSE rest
             /\ UNCHANGED <<buf, tab, pc>>
          ELSE IF tab[P].pe = None THEN
             \* handleUnknownByID (packed kind: no retired ids)
             IF p = Home THEN
                \* the home holds it (bufferForLocation, id path)
                /\ Hold(p, BufRec(m, h1, r.b))
                /\ net' = rest
                /\ UNCHANGED dcount
             ELSE IF ~mlarge[m] THEN
                /\ net' = rest \cup {AppMsg(Home, m, h1, b1)}
                /\ UNCHANGED <<buf, tab, pc, dcount>>
             ELSE
                /\ Hold(p, BufRec(m, h1, r.b))
                /\ net' = rest
                /\ UNCHANGED dcount
          ELSE
             \* follow the table; after two tries go home, except at the home
             LET dest == IF h1 > 1 /\ p # Home THEN Home ELSE tab[P].pe IN
             /\ net' = rest \cup {AppMsg(dest, m, h1, b1)}
             /\ UNCHANGED <<buf, tab, pc, dcount>>
    /\ UNCHANGED <<holder, epoch, migs, creates, dels, pendReq, sent, msrc, mlarge, hl, dead>>

(* CkLocMgr::updateLocation(idx, e) run locally after createLocal / immigrate: *)
(* notifyListeners(idx) flushes what this PE holds toward e.pe (= this PE),    *)
(* and at the home answers bufferedLocationRequests.                           *)
LocalUpdateMsgs(p, e, l) ==
    Flush(p, p) \cup (IF p = Home THEN {UpdMsgL(q, p, e, TRUE, l) : q \in pendReq \ {p}} ELSE {})
LocalUpdateState(p) ==
    /\ buf' = [buf EXCEPT ![p] = {}]
    /\ pendReq' = IF p = Home THEN {} ELSE pendReq

(* cklocation.C CkLocMgr::createLocal(idx, ..., epoch 0) on c, from an insert:  *)
(* CkLocCache::insert = LocTable::store (W1) + wakes; updateLocation(idx, e)    *)
(* locally; informHome(idx, e) unless the home is c.                           *)
Create(c) ==
    /\ holder = NotCreated /\ creates < MaxCreate /\ pc[c].kind = "idle"
    \* Sync: the contract.  Held application messages and held requests may remain.
    /\ (Sync => \A r \in net : r.type = "app")
    /\ LET P == Proc(c)
           l == creates + 1  \* this creation's life
       IN
       /\ holder' = c
       /\ epoch' = 0
       /\ creates' = l
       /\ tab' = [tab EXCEPT ![P] = Store(P, c, 0)]
       /\ net' = net \cup LocalUpdateMsgs(c, 0, l) \cup Wakes(tab[P].w, c)
                 \cup (IF Home # c THEN {UpdMsgL(Home, c, 0, TRUE, l)} ELSE {})
       /\ hl' = IF UseRec /\ c = Home THEN Upsert(hl, HomeRec(c, 0, l)) ELSE hl
       /\ LocalUpdateState(c)
    /\ UNCHANGED <<migs, dels, pc, sent, msrc, mlarge, dcount, dead>>

(* cklocation.C CkLocMgr::reclaim on h after ckDestroy: the epoch is read, then   *)
(* removeFromTable clears the process entry while it names h (W5, waiter bits   *)
(* kept), then reclaimRemote(idx, h, epoch, life) goes to the home unless h is  *)
(* the home (noteDead; Fix: homeNoteDeath here).  The element can be created    *)
(* again (packed kind: same id, epoch 0).                                      *)
Delete(h) ==
    /\ holder = h /\ dels < MaxDel /\ pc[h].kind = "idle"
    /\ (AppSync => (\A r \in net : r.type # "app") /\ (\A p \in PEs : buf[p] = {} /\ pc[p].kind = "idle"))
    /\ LET P == Proc(h)
           T1 == IF tab[P].pe = h THEN Cleared(tab[P]) ELSE tab[P]
           hl1 == IF UseRec /\ h = Home THEN {x \in hl : x.life # creates} ELSE hl
       IN
       /\ hl' = hl1
       /\ dead' = IF (UseRec \/ UseDead) /\ h = Home THEN dead \cup {creates} ELSE dead
       /\ IF h = Home
            THEN AfterClear(T1, hl1, net)
            ELSE /\ tab' = [tab EXCEPT ![P] = T1]
                 /\ net' = net \cup {DeadMsg(h, epoch)}
                 /\ UNCHANGED <<buf, pendReq>>
    /\ holder' = NotCreated
    /\ dels' = dels + 1
    /\ UNCHANGED <<epoch, migs, creates, pc, sent, msrc, mlarge, dcount>>

(* CkLocMgr::reclaimRemote at the home: clear the process entry while it names  *)
(* the PE and epoch the life died with (cache->erase(id, deletedOnPe, epoch)).  *)
(* Fix (homeNoteDeath): the life is dead; also clear while the entry names the  *)
(* location the home recorded for that life; forget the life; reinstall.       *)
RecvDead(r) ==
    /\ r \in net /\ r.type = "dead"
    /\ pc[Home].kind = "idle"
    /\ LET P == Proc(Home)
           T == tab[P]
           known == IF UseRec THEN {x \in hl : x.life = r.life} ELSE {}
           hl1 == hl \ known
           \* Fix: what a PE of the home's process wrote about itself stays (it cleared
           \* its own entry before any notice was sent).
           clear == /\ ~OwnWrite(P, T)
                    /\ \/ UseDead  \* no record: clear whatever stands (the contract excludes a newer life)
                       \/ (T.pe = r.pe /\ T.ep = r.ep) \/ \E x \in known : T.pe = x.pe /\ T.ep = x.ep
           T1 == IF clear THEN Cleared(T) ELSE T
       IN
       /\ dead' = IF UseRec \/ UseDead THEN dead \cup {r.life} ELSE dead
       /\ hl' = hl1
       /\ AfterClear(T1, hl1, net \ {r})
    /\ UNCHANGED <<holder, epoch, migs, creates, dels, pc, sent, msrc, mlarge, dcount>>

(* cklocation.C CkLocMgr::emigrate on H, to T: recordEmigration (W2, the entry *)
(* must name H: invariant HolderEntryExact), then the migration message with   *)
(* the new epoch, then informHome(idx, {id, T, e+1}) unless the home is H or T. *)
Emigrate(H, T) ==
    /\ holder = H /\ H # T /\ migs < MaxMig /\ pc[H].kind = "idle"
    /\ LET P == Proc(H)
           e1 == epoch + 1
       IN
       /\ tab' = IF Mutation = 1 THEN tab
                 ELSE [tab EXCEPT ![P] = [pe |-> T, ep |-> e1, w |-> tab[P].w, own |-> TRUE]]
       /\ net' = net \cup {ImmMsg(T, e1)}
                     \cup (IF Home \notin {H, T} THEN {UpdMsg(Home, T, e1, TRUE)} ELSE {})
       /\ epoch' = e1
       \* informHome with the home = H: recorded on the spot, no message
       /\ hl' = IF UseRec /\ H = Home THEN Upsert(hl, HomeRec(T, e1, creates)) ELSE hl
    /\ holder' = None
    /\ migs' = migs + 1
    /\ UNCHANGED <<creates, dels, pc, buf, pendReq, sent, msrc, mlarge, dcount, dead>>

(* cklocation.C CkLocMgr::immigrate -> createLocal(notifyHome = false):        *)
(* CkLocCache::insert = store (W3) + wakes; updateLocation(idx, e) locally.     *)
RecvImm(r) ==
    /\ r \in net /\ r.type = "imm"
    /\ LET T == r.to
           P == Proc(T)
       IN
       /\ pc[T].kind = "idle"
       /\ holder' = T
       /\ tab' = [tab EXCEPT ![P] = Store(P, T, r.ep)]
       /\ LocalUpdateState(T)
       /\ net' = (net \ {r}) \cup LocalUpdateMsgs(T, r.ep, creates)
                 \cup Wakes(tab[P].w, T)
       /\ hl' = IF UseRec /\ T = Home THEN Upsert(hl, HomeRec(T, r.ep, creates)) ELSE hl
    /\ UNCHANGED <<epoch, migs, creates, dels, pc, sent, msrc, mlarge, dcount, dead>>

(* An update arriving at p.  idx = TRUE: CkLocMgr::updateLocation(idx, e):      *)
(* cache->updateLocation(e) (W4 + wakes if written), then notifyListeners(idx)  *)
(* UNCONDITIONALLY (flush toward e.pe) and, at the home, answer held requests.  *)
(* idx = FALSE: CkLocCache::updateLocation(e): W4; flush only if written.        *)
(* Fix (homeNoteLocation): at the home an update by index of a life known dead   *)
(* is dropped, after clearing the entry while it names that update's location;  *)
(* any other is recorded for its life first.                                    *)
RecvUpd(r) ==
    /\ r \in net /\ r.type = "upd"
    /\ LET p == r.to
           P == Proc(p)
           rest == net \ {r}
           atHome == UseRec /\ p = Home /\ r.idx
           wr == Written(P, r.ep)
           flush == wr \/ r.idx
       IN
       /\ pc[p].kind = "idle"
       /\ IF Fix /\ ~r.idx /\ P = Proc(Home) THEN
             \* a reply or repair by id in the home's process: dropped; the home's
             \* by-index path and the process's own writes keep this entry
             /\ net' = rest
             /\ UNCHANGED <<tab, buf, pendReq, hl>>
          ELSE IF (atHome /\ (r.life \in dead \/ Stale(r))) \/ (UseDead /\ p = Home /\ r.idx /\ r.life \in dead) THEN
             LET T == tab[P]
                 T1 == IF T.pe = r.pe /\ T.ep = r.ep /\ ~OwnWrite(P, T) THEN Cleared(T) ELSE T
             IN /\ AfterClear(T1, hl, rest)
                /\ UNCHANGED hl
          ELSE
             /\ hl' = IF atHome THEN Upsert(hl, HomeRec(r.pe, r.ep, r.life)) ELSE hl
             /\ tab' = IF wr THEN [tab EXCEPT ![P] = Recv4(r.pe, r.ep)] ELSE tab
             /\ buf' = IF flush THEN [buf EXCEPT ![p] = {}] ELSE buf
             /\ pendReq' = IF p = Home /\ r.idx THEN {} ELSE pendReq
             /\ net' = rest
                       \cup (IF wr THEN Wakes(tab[P].w, p) ELSE {})
                       \cup (IF flush THEN Flush(p, r.pe) ELSE {})
                       \cup (IF p = Home /\ r.idx
                               THEN {UpdMsgL(q, r.pe, r.ep, TRUE, r.life) : q \in pendReq \ {p}} ELSE {})
    /\ UNCHANGED <<holder, epoch, migs, creates, dels, pc, sent, msrc, mlarge, dcount, dead>>

(* CkLocCache::requestLocation(id, peToTell) at the home: answer by id from the *)
(* entry, or hold the request (CkLocMgr::requestLocation(idx, peToTell)).       *)
RecvReq(r) ==
    /\ r \in net /\ r.type = "req"
    /\ pc[Home].kind = "idle"
    /\ LET e == tab[Proc(Home)] IN
       IF e.pe # None
         THEN /\ net' = (net \ {r}) \cup {[type |-> "upd", to |-> r.tell, pe |-> e.pe, ep |-> e.ep,
                                             idx |-> FALSE, life |-> 0]}
              /\ UNCHANGED pendReq
         ELSE /\ net' = net \ {r}
              /\ pendReq' = pendReq \cup {r.tell}
    /\ UNCHANGED <<holder, epoch, migs, creates, dels, tab, pc, buf, sent, msrc, mlarge, dcount, hl, dead>>

(* CkLocCache::wake(id) on p: location known -> flush; else (packed kind: not   *)
(* a retired id) re-register while messages are held.                          *)
RecvWake(r) ==
    /\ r \in net /\ r.type = "wake"
    /\ LET p == r.to
           P == Proc(p)
           rest == net \ {r}
       IN
       /\ pc[p].kind = "idle"
       /\ IF tab[P].pe # None THEN
             /\ net' = rest \cup Flush(p, tab[P].pe)
             /\ buf' = [buf EXCEPT ![p] = {}]
             /\ UNCHANGED <<tab, pc>>
          ELSE IF buf[p] # {} THEN
             /\ net' = rest
             /\ IF Mutation = 2
                  THEN /\ pc' = [pc EXCEPT ![p] = WakePC(tab[P].w = {})]
                       /\ UNCHANGED tab
                  ELSE /\ tab' = [tab EXCEPT ![P].w = @ \cup {p}]
                       /\ pc' = [pc EXCEPT ![p] = WakePC(tab[P].w = {})]
             /\ UNCHANGED buf
          ELSE
             /\ net' = rest
             /\ UNCHANGED <<tab, pc, buf>>
    /\ UNCHANGED <<holder, epoch, migs, creates, dels, pendReq, sent, msrc, mlarge, dcount, hl, dead>>

Recv(r) == RecvApp(r) \/ RecvImm(r) \/ RecvUpd(r) \/ RecvReq(r) \/ RecvWake(r) \/ RecvDead(r)

Next ==
    \/ \E m \in MsgIds, s \in PEs, large \in BOOLEAN : Send(m, s, large)
    \/ \E c \in PEs : Create(c)
    \/ \E h \in PEs : Delete(h)
    \/ \E H, T \in PEs : Emigrate(H, T)
    \/ \E r \in net : Recv(r)
    \/ \E p \in PEs : Reread(p)

\* Every message in flight is eventually received, every split handler
\* completes, and the element is eventually created.
Fairness ==
    /\ \A r \in MsgUniverse : WF_vars(Recv(r))
    /\ \A p \in PEs : WF_vars(Reread(p))
    /\ WF_vars(\E c \in PEs : Create(c))

Spec == Init_ /\ [][Next]_vars /\ Fairness

-----------------------------------------------------------------------------
(* Safety *)

HeldInPC(m) == {p \in PEs : pc[p].kind = "hold" /\ pc[p].rec.mid = m}

Copies(m) ==
      Cardinality({r \in net : r.type = "app" /\ r.mid = m})
    + Cardinality(UNION {{p} \X {r \in buf[p] : r.mid = m} : p \in PEs})
    + Cardinality(HeldInPC(m))
    + dcount[m]

\* Every sent message exists exactly once: in flight, held, mid-handler, or delivered.
ExactlyOnce == \A m \in MsgIds : Copies(m) = IF m \in sent THEN 1 ELSE 0

\* The element is in exactly one place: on holder, or in one immigrate message.
OneElement ==
    LET imms == {r \in net : r.type = "imm"} IN
    IF holder = None THEN Cardinality(imms) = 1 ELSE imms = {}

\* The holder's process entry is exact (recordEmigration's CmiAbort cannot fire,
\* and a same-process sender reads the right PE); no entry is ahead of the element.
HolderEntryExact ==
    holder \in PEs => tab[Proc(holder)].pe = holder /\ tab[Proc(holder)].ep = epoch

\* The holder's process entry, when it has a location, is the holder's: a stale
\* update of an earlier life has not overwritten the live element's own entry
\* (the ordering reported by the independent model, README finding 5).  Finding 1
\* leaves the entry empty and does not violate this one.
HolderEntryNotStale ==
    holder \in PEs /\ tab[Proc(holder)].pe # None =>
        tab[Proc(holder)].pe = holder /\ tab[Proc(holder)].ep = epoch

\* Once every death notice has been received, no entry of a dead life stands at
\* the holder's process (Fix: a dead life's message arriving after its notice is
\* dropped; one that arrived before was cleared by the notice).
StaleOnlyBeforeNotice == (\A r \in net : r.type # "dead") => HolderEntryNotStale

\* Once every notice, home update and migration message has been received, the
\* holder's process entry is exact: the windows of section 9 have closed.
SettledHolderExact == (\A r \in net : r.type \notin {"upd", "dead", "imm"}) => HolderEntryExact

\* Without deaths only: a re-creation restarts at epoch 0 while bystander
\* processes may still hold the old life's epoch (chapter section 3.5).
EpochOK == dels = 0 => \A P \in Procs : tab[P].pe # None => tab[P].ep <= epoch

\* A held message always has something pending that will release it: the rank's
\* waiter bit, a wake, an update or the element itself on the way to it, or the
\* rank is mid-request.
WaiterCoverage ==
    \A p \in PEs : buf[p] # {} =>
        \/ p \in tab[Proc(p)].w
        \/ WakeMsg(p) \in net
        \/ pc[p].kind # "idle"
        \/ \E r \in net : r.type \in {"upd", "imm"} /\ r.to = p

\* When a process waits with no location and the home is elsewhere, a request
\* from that process is out, or held at the home, or being answered, or about
\* to be sent by the first registrant.
RequestOutstanding ==
    \A P \in Procs : (tab[P].w # {} /\ tab[P].pe = None /\ Proc(Home) # P) =>
        \/ \E q \in Ranks(P) : ReqMsg(q) \in net \/ q \in pendReq
        \/ \E r \in net : r.type = "upd" /\ Proc(r.to) = P
        \/ \E q \in Ranks(P) : pc[q].kind # "idle" /\ pc[q].first

Inv == TypeOK /\ ExactlyOnce /\ OneElement /\ HolderEntryExact /\ EpochOK
       /\ WaiterCoverage /\ RequestOutstanding

\* Table epochs never move back.
TableMonotone ==
    [][dels = 0 /\ dels' = 0 =>
        \A P \in Procs : tab[P].pe # None => tab'[P].pe # None /\ tab'[P].ep >= tab[P].ep]_vars

(* Liveness *)

EventuallyDelivered == \A m \in MsgIds : (m \in sent) ~> (dcount[m] = 1)

HomeConverges ==
    <>[](holder \in PEs /\ tab[Proc(Home)].pe = holder /\ tab[Proc(Home)].ep = epoch)

Quiesces == <>[](net = {} /\ \A p \in PEs : buf[p] = {} /\ pc[p].kind = "idle")
\* The network drains: every message comes to rest, delivered or held (a message
\* to an element that died and is not re-created is held at the home).
NetDrains == <>[](net = {})

-----------------------------------------------------------------------------
(* Probe: expected to FAIL; its trace shows how far a message is forwarded. *)
BoundedForwarding == \A r \in net : r.type = "app" => r.b < BCap

=============================================================================
