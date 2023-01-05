
#include "tinyos.h"
#include "kernel_dev.h"
#include "kernel_streams.h"
#include "kernel_cc.h"
#include "kernel_sched.h"

// port map
SCB* PORTMAP[MAX_PORT];
static int first_call = 1;

file_ops socket_file_ops = {
	.Open  = NULL,
	.Read  = socket_read,
	.Write = socket_write,
	.Close = socket_close
};

Fid_t sys_Socket(port_t port)
{		
	if(port == NOPORT)
		return 0;


	// if input port is invalid, reurn NOFILE
	if (port < 0 || port > MAX_PORT)
		return NOFILE;

	FCB* fcb;
	Fid_t fid;
	int k = FCB_reserve(1,&fid,&fcb);
	// if FCB_reserve returns 0 ,there is no FCB reservation hence the return value NOFILE.
	if(k == 0)
		return NOFILE;

	// if this is the first time we call sys_Socket , initialize the port map
	if (first_call) {
		for (int i=0; i < MAX_PORT; i++) {
			PORTMAP[i] = NULL;
		}
		first_call--;
	}

	/*NEW SCB*/
	// create the SCB
	SCB* socket_cb = (SCB*)xmalloc(sizeof(SCB));

	// initialize new socket control blocks' fields
	socket_cb->fcb  	= fcb;
	socket_cb->type 	= SOCKET_UNBOUND;
	socket_cb->port 	= port;
	socket_cb->refcount = 1;

	// initialize file control block fields
	fcb->streamobj = socket_cb;
	fcb->streamfunc = &socket_file_ops;

	return fid;
}

int sys_Listen(Fid_t sock)
{	
	// invalid file ID
	if (sock > MAX_FILEID || sock < 0)
		return -1;

	FCB* fcb = get_fcb(sock);

	// if file control block is null, there is no socket
	if (fcb == NULL)
		return -1;

	SCB* p_scb = fcb->streamobj;
	// if there is no streamobj in fcb, there is no socket
	if (p_scb == NULL)
		return -1;

	// port and socket inspection
	if(PORTMAP[p_scb->port] != NULL)	// socket is already initialized
		return -1;
	if(p_scb->port < 0 || p_scb->port > MAX_PORT)	// socket must have a legal port number
		return -1;
	if(p_scb->type != SOCKET_UNBOUND)	// socket already a listener
		return -1;

	// bind socket to port
	PORTMAP[p_scb->port] = p_scb;

	// adjust socket fields and union
	p_scb->type = SOCKET_LISTENER;
	p_scb->s_listener.req_available = COND_INIT;
	rlnode_init(&(p_scb->s_listener.queue),NULL);

	return 0;
}


Fid_t sys_Accept(Fid_t lsock)
{	
	// invalid file ID
	if (lsock > MAX_FILEID || lsock < 0)
		return NOFILE;


	FCB* fcb = get_fcb(lsock);

	// if file control block is null, there is no socket
	if (fcb == NULL)
		return NOFILE;
	// if streamfunc of file control block is invalid , the fcb is not an operational socket
	if (fcb->streamfunc != &socket_file_ops)
		return NOFILE;

	SCB* p_scb = fcb->streamobj;

	// do all checks as necessary
	if (p_scb == NULL)
		return NOFILE;
	if (p_scb->port <= NOFILE || p_scb->port > MAX_PORT)
		return NOFILE;
	if (p_scb->type != SOCKET_LISTENER)
		return NOFILE;
	if ( ( PORTMAP[p_scb->port] )->type != SOCKET_LISTENER )
		return NOFILE;


	// INCREASE REFERENCE COUNT
	p_scb->refcount++;

	/*While there is no request , wait*/
	while ( rlist_len(&p_scb->s_listener.queue) == 0 ){

		//check whether listener is still alive
		if ( PORTMAP[p_scb->port] == NULL ) 
			return NOFILE;

		kernel_wait(&p_scb->s_listener.req_available, SCHED_IO);

	}

	/* ESTABLISH CONNECTION */
	CONNECTION_REQUEST* request = rlist_pop_front(&p_scb->s_listener.queue)->connection_request;

	request->admitted = 1;

	// get peer 1 from connection
	SCB* peer1 = request->peer;

	if(peer1 == NULL) 
		return NOFILE;

	//create a socket with port from peer 1
	Fid_t desc = sys_Socket(peer1->port);
	FCB* f = get_fcb(desc);

	if(f == NULL)
		return NOFILE;

	SCB* peer2 = f->streamobj;

	if(peer2 == NULL)
		return NOFILE;




	return NOFILE;
}


int sys_Connect(Fid_t sock, port_t port, timeout_t timeout)
{
	return -1;
}


int sys_ShutDown(Fid_t sock, shutdown_mode how)
{
	return -1;
}




// socket read/write/close

int socket_read(void* read, char* buf, uint size){
	return -1;
}

int socket_write(void* write, const char* buf, uint size){
	return -1;
}

int socket_close(void* fid){
	return -1;
}