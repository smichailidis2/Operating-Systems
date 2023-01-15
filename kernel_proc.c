#include <assert.h>
#include "kernel_cc.h"
#include "kernel_proc.h"
#include "kernel_streams.h"


/* 
 The process table and related system calls:
 - Exec
 - Exit
 - WaitPid
 - GetPid
 - GetPPid
 */

/* The process table */
PCB PT[MAX_PROC];
unsigned int process_count;

PCB* get_pcb(Pid_t pid)
{
  return PT[pid].pstate==FREE ? NULL : &PT[pid];
}

Pid_t get_pid(PCB* pcb)
{
  return pcb==NULL ? NOPROC : pcb-PT;
}

/* Initialize a PCB */
static inline void initialize_PCB(PCB* pcb)
{
  pcb->pstate = FREE;
  pcb->argl = 0;
  pcb->args = NULL;

  for(int i=0;i<MAX_FILEID;i++)
    pcb->FIDT[i] = NULL;


  pcb->thread_count = 0;
  rlnode_init(&pcb->ptcb_list,NULL);


  rlnode_init(& pcb->children_list, NULL);
  rlnode_init(& pcb->exited_list, NULL);
  rlnode_init(& pcb->children_node, pcb);
  rlnode_init(& pcb->exited_node, pcb);
  pcb->child_exit = COND_INIT;
  
}


static PCB* pcb_freelist;

void initialize_processes()
{
  /* initialize the PCBs */
  for(Pid_t p=0; p<MAX_PROC; p++) {
    initialize_PCB(&PT[p]);
  }

  /* use the parent field to build a free list */
  PCB* pcbiter;
  pcb_freelist = NULL;
  for(pcbiter = PT+MAX_PROC; pcbiter!=PT; ) {
    --pcbiter;
    pcbiter->parent = pcb_freelist;
    pcb_freelist = pcbiter;
  }

  process_count = 0;

  /* Execute a null "idle" process */
  if(Exec(NULL,0,NULL)!=0)
    FATAL("The scheduler process does not have pid==0");
}


/*
  Must be called with kernel_mutex held
*/
PCB* acquire_PCB()
{
  PCB* pcb = NULL;

  if(pcb_freelist != NULL) {
    pcb = pcb_freelist;
    pcb->pstate = ALIVE;
    pcb_freelist = pcb_freelist->parent;
    process_count++;
  }

  return pcb;
}

/*
  Must be called with kernel_mutex held
*/
void release_PCB(PCB* pcb)
{
  pcb->pstate = FREE;
  pcb->parent = pcb_freelist;
  pcb_freelist = pcb;
  process_count--;
}


/*
 *
 * Process creation
 *
 */

/*
  This function is provided as an argument to spawn,
  to execute the main thread of a process.
*/
void start_main_thread()
{
  int exitval;

  Task call =  CURPROC->main_task;
  int argl = CURPROC->argl;
  void* args = CURPROC->args;

  exitval = call(argl,args);
  Exit(exitval);
}


/*
  System call to create a new process.
 */
Pid_t sys_Exec(Task call, int argl, void* args)
{
  PCB *curproc, *newproc;
  
  /* The new process PCB */
  newproc = acquire_PCB();

  if(newproc == NULL) goto finish;  /* We have run out of PIDs! */

  if(get_pid(newproc)<=1) {
    /* Processes with pid<=1 (the scheduler and the init process) 
       are parentless and are treated specially. */
    newproc->parent = NULL;
  }
  else
  {
    /* Inherit parent */
    curproc = CURPROC;

    /* Add new process to the parent's child list */
    newproc->parent = curproc;
    rlist_push_front(& curproc->children_list, & newproc->children_node);

    /* Inherit file streams from parent */
    for(int i=0; i<MAX_FILEID; i++) {
       newproc->FIDT[i] = curproc->FIDT[i];
       if(newproc->FIDT[i])
          FCB_incref(newproc->FIDT[i]);
    }
  }


  /* Set the main thread's function */
  newproc->main_task = call;

  /* Copy the arguments to new storage, owned by the new process */
  newproc->argl = argl;
  if(args!=NULL) {
    newproc->args = malloc(argl);
    memcpy(newproc->args, args, argl);
  }
  else
    newproc->args=NULL;

  /* 
    Create and wake up the thread for the main function. This must be the last thing
    we do, because once we wakeup the new thread it may run! so we need to have finished
    the initialization of the PCB.
   */
  if(call != NULL) {
    newproc->main_thread = spawn_thread(newproc, start_main_thread);
    newproc->thread_count++;
    acquire_ptcb(newproc->main_thread, call, argl, args);
    wakeup(newproc->main_thread);
  }


finish:
  return get_pid(newproc);
}


/* System call */
Pid_t sys_GetPid()
{
  return get_pid(CURPROC);
}


Pid_t sys_GetPPid()
{
  return get_pid(CURPROC->parent);
}


static void cleanup_zombie(PCB* pcb, int* status)
{
  if(status != NULL)
    *status = pcb->exitval;

  rlist_remove(& pcb->children_node);
  rlist_remove(& pcb->exited_node);

  release_PCB(pcb);
}


static Pid_t wait_for_specific_child(Pid_t cpid, int* status)
{

  /* Legality checks */
  if((cpid<0) || (cpid>=MAX_PROC)) {
    cpid = NOPROC;
    goto finish;
  }

  PCB* parent = CURPROC;
  PCB* child = get_pcb(cpid);
  if( child == NULL || child->parent != parent)
  {
    cpid = NOPROC;
    goto finish;
  }

  /* Ok, child is a legal child of mine. Wait for it to exit. */
  while(child->pstate == ALIVE)
    kernel_wait(& parent->child_exit, SCHED_USER);
  
  cleanup_zombie(child, status);
  
finish:
  return cpid;
}


static Pid_t wait_for_any_child(int* status)
{
  Pid_t cpid;

  PCB* parent = CURPROC;

  /* Make sure I have children! */
  int no_children, has_exited;
  while(1) {
    no_children = is_rlist_empty(& parent->children_list);
    if( no_children ) break;

    has_exited = ! is_rlist_empty(& parent->exited_list);
    if( has_exited ) break;

    kernel_wait(& parent->child_exit, SCHED_USER);    
  }

  if(no_children)
    return NOPROC;

  PCB* child = parent->exited_list.next->pcb;
  assert(child->pstate == ZOMBIE);
  cpid = get_pid(child);
  cleanup_zombie(child, status);

  return cpid;
}


Pid_t sys_WaitChild(Pid_t cpid, int* status)
{
  /* Wait for specific child. */
  if(cpid != NOPROC) {
    return wait_for_specific_child(cpid, status);
  }
  /* Wait for any child */
  else {
    return wait_for_any_child(status);
  }

}


void sys_Exit(int exitval)
{

  PCB *curproc = CURPROC;  /* cache for efficiency */

  /* First, store the exit status */
  curproc->exitval = exitval;

  /* 
    Here, we must check that we are not the init task. 
    If we are, we must wait until all child processes exit. 
   */
  if(get_pid(curproc)==1) {

    while(sys_WaitChild(NOPROC,NULL)!=NOPROC);

  } 

  sys_ThreadExit(exitval);  
}

// ****** sys_info *******

file_ops procinfo_ops = {
  .Open  = NULL,
  .Read  = procinfo_read,
  .Write = NULL,
  .Close = procinfo_close
};



Fid_t sys_OpenInfo()
{
  Fid_t fid;
  FCB* fcb;
  if(!FCB_reserve(1,&fid,&fcb))
    return -1;
  

  // new proc info control block
  PROCINF_CB* proc_info =(PROCINF_CB*)xmalloc(sizeof(PROCINF_CB));
  fcb -> streamobj  = proc_info;
  fcb -> streamfunc = &procinfo_ops;

  // cursor of proc_info is the first PCB in PT; Cursor then spans the proc table.
  proc_info -> cursor   = get_pcb(0);

  proc_info -> procinfo = NULL;

  return fid;
}



int procinfo_read(void* pr,char* buf, unsigned int size){

  PROCINF_CB* procinfocb =(PROCINF_CB*)pr;

  if(procinfocb==NULL || procinfocb->cursor== NULL)
    return -1;
  

  // Initialize procinfo inside proc info control block
  procinfocb -> procinfo = (procinfo*)xmalloc(sizeof(procinfo));
  procinfocb -> procinfo -> pid  = get_pid(procinfocb->cursor);           // get pid from cursor
  procinfocb -> procinfo -> ppid = get_pid(procinfocb -> cursor->parent); // get pid from parent of cursor


  // check for cursor state
  if(procinfocb -> cursor->pstate == ALIVE){
    procinfocb -> procinfo->alive=1; 
  }else{
    procinfocb -> procinfo->alive=0;
  }

  // init procinfo
  procinfocb -> procinfo -> thread_count = procinfocb -> cursor->thread_count;
  procinfocb -> procinfo -> main_task    = procinfocb -> cursor->main_task;
  procinfocb -> procinfo -> argl = procinfocb -> cursor -> argl; 

  // if argument size to copy is greater or equal to Max arg size, copy only Max arg size
  int args_size = (procinfocb->cursor->argl >= PROCINFO_MAX_ARGS_SIZE) ? PROCINFO_MAX_ARGS_SIZE : (procinfocb->cursor->argl);

  // copy data
  memcpy(procinfocb->procinfo->args, procinfocb->cursor->args, args_size);
  //memcpy(buf, (char*)&procinfocb->procinfo, sizeof(procinfo));
  memcpy(buf, procinfocb->procinfo,size);

  // procinfo has done its job
  free(procinfocb->procinfo);

  // increment cursor to the next PCB
  procinfocb->cursor = get_pcb( get_pid(procinfocb->cursor) + 1 );

  return size;
}




int procinfo_close(void* pr){
  PROCINF_CB* procinfo_cb = (PROCINF_CB*) pr;

  if(procinfo_cb == NULL)
    return -1;
  
  free(procinfo_cb);

  return 0;
}


/*
 * Creating create_ptcb
 */

void acquire_ptcb(TCB* tcb, Task task, int argl, void* args) {

  PTCB* ptcb = (PTCB*)xmalloc(sizeof(PTCB)); // allocating memory for a PTCB

  ptcb->tcb = tcb;  
  tcb->ptcb = ptcb;

  ptcb->task = task;
  ptcb->argl = argl;
  ptcb->args = args;

  ptcb->exited = 0;
  ptcb->exitval = 0;
  ptcb->detached = 0;

  ptcb->exit_cv = COND_INIT;
  ptcb->refcount = 1;

  rlnode_init(&ptcb->ptcb_list_node, ptcb);

  rlist_push_back(&tcb->owner_pcb->ptcb_list, &ptcb->ptcb_list_node);
  
}

void increase_refcount(PTCB* ptcb) {
  ptcb->refcount++;
}

void decrease_refcount(PTCB* ptcb) {
  ptcb->refcount--;
}

void start_main_ptcb_thread() {

  if(cur_thread() != NULL){
    int exitval;

    Task call = cur_thread()->ptcb->task;
    int argl = cur_thread()->ptcb->argl;
    void* args = cur_thread()->ptcb->args;

    exitval = call(argl, args);
    sys_ThreadExit(exitval);
  }
}