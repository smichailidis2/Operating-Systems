
#include "tinyos.h"
#include "kernel_dev.h"
#include "kernel_streams.h"
#include "kernel_cc.h"
#include "kernel_sched.h"
#include "kernel_proc.h"

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

	// if input port is invalid, reurn NOFILE
	if (port < 0 || port > MAX_PORT)
		return NOFILE;

	FCB* fcb = NULL;
	Fid_t fid = -1;
	int k = FCB_reserve(1,&fid,&fcb);
	// if FCB_reserve returns 0 ,there is no FCB reservation.
	if(!k)
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
	if(p_scb->port <= 0 || p_scb->port > MAX_PORT)	// socket must have a legal port number
		return -1;
	if(p_scb->type != SOCKET_UNBOUND)	// socket already a listener
		return -1;

	// adjust socket fields and union
	p_scb->type = SOCKET_LISTENER;
	p_scb->s_listener.req_available = COND_INIT;
	rlnode_init(&(p_scb->s_listener.queue),NULL);

	// bind socket to port
	PORTMAP[p_scb->port] = p_scb;

	return 0;
}


Fid_t sys_Accept(Fid_t lsock)
{	
	// invalid file ID
	if (lsock > MAX_FILEID || lsock < 0)
		return NOFILE;

	// check if process has available fids 
	PCB* pcb = get_pcb(lsock);
	if (pcb == NULL)
		return NOFILE;
	uint c = 0;
	for (uint j = 0; j < MAX_FILEID; j++) {
		if(pcb->FIDT[j] != NULL)
			c++;
	}
	if (c == MAX_FILEID)
		return NOFILE;
	// ===================================


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
	if (p_scb->port <= NOPORT || p_scb->port > MAX_PORT)
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
	if (desc == NOFILE)
		return NOFILE;

	FCB* f = get_fcb(desc);

	if(f == NULL)
		return NOFILE;

	SCB* peer2 = f->streamobj;

	if(peer2 == NULL)
		return NOFILE;

	peer1->type = SOCKET_PEER;
	peer2->type = SOCKET_PEER;


	// connect 2 sockets
	peer1->s_peer.peer = peer2;
	peer2->s_peer.peer = peer1;

	/* PIPE CREATION */
	FCB* t_fs1;
	FCB* t_fs2;

	// get file control blocks of peers
	t_fs1 = peer1->fcb;
	t_fs2 = peer2->fcb;

	// create pipes
	pipe_cb* pipe1 = (pipe_cb*)xmalloc(sizeof(pipe_cb));
	pipe_cb* pipe2 = (pipe_cb*)xmalloc(sizeof(pipe_cb));

	// --- INIT PIPE CBs ---
	// PIPE 1)
	pipe1->reader = t_fs1;
	pipe1->writer = t_fs2;

	pipe1->has_space = COND_INIT;
	pipe1->has_data = COND_INIT;
	pipe1->w_pos = 0;
	pipe1->r_pos = 0;

	// PIPE 2)
	pipe2->reader = t_fs2;
	pipe2->writer = t_fs1;

	pipe2->has_space = COND_INIT;
	pipe2->has_data = COND_INIT;
	pipe2->w_pos = 0;
	pipe2->r_pos = 0;
	// -----------------------

	peer1->s_peer.write = pipe1;
	peer1->s_peer.read  = pipe2;

	peer2->s_peer.write = pipe2;
	peer2->s_peer.read  = pipe1;

	kernel_signal(&request->connected_cv);
	p_scb->refcount--;

	if (p_scb->refcount == 0)
		free(p_scb);

	return desc;
}


int sys_Connect(Fid_t sock, port_t port, timeout_t timeout)
{	
	// check illegal file id
	if (sock<0 || sock>MAX_FILEID)
		return -1;

	FCB* f = get_fcb(sock);

	SCB* p_socket = f->streamobj;

	// Check the following:
	//1. If file stream is NULL
	//2. If stream object is NULL
	//3. If socket to be connected is already a peer or a listener
	if (f == NULL || p_socket == NULL || p_socket->type != SOCKET_UNBOUND) 
		return -1;
	if (port < 0 || port > MAX_PORT)
		return -1;
	if (PORTMAP[port] == NULL) 
		return -1;

	/* Establish the connection */
	CONNECTION_REQUEST* request = (CONNECTION_REQUEST*)xmalloc(sizeof(CONNECTION_REQUEST));
	SCB* listener = PORTMAP[port];

	//init request
	request->admitted = 0;
	request->peer = p_socket;
	request->connected_cv = COND_INIT;
	rlnode_init(&request->queue_node, request);

	// add request to the listener's request queue
	rlist_push_back(&listener->s_listener.queue, &request->queue_node);

	// signal the listener
	kernel_signal(&listener->s_listener.req_available);

	listener->refcount++;
	
	while (!request->admitted) {
		int retval = kernel_timedwait(&request->connected_cv, SCHED_IO, timeout);
		
		// request timed out
		if(!retval)
			return -1;
	}

	p_socket->refcount--;
	if( p_socket->refcount == 0 )
		free(p_socket);

	if ( request->admitted == 0 )
		return -1;

	return 0;
}


int sys_ShutDown(Fid_t sock, shutdown_mode how)
{
	// illegal shutdown_mode arg value
	if (how < 1 || how > 3)
		return -1;

	// illegal file id
	if (sock < 0 || sock > MAX_FILEID)
		return -1;


	FCB* fcb = get_fcb(sock);

	if(fcb == NULL)
		return -1;

	SCB* p_socket = fcb->streamobj;

	// shut down can only be used on a peer socket
	if (p_socket->type != SOCKET_PEER)
		return -1;

	switch (how) {

		case SHUTDOWN_READ:
			if( pipe_reader_close(p_socket->s_peer.read) != 0 )
				return -1;
			p_socket->s_peer.read = NULL;
			break;
		case SHUTDOWN_WRITE:
			if( pipe_writer_close(p_socket->s_peer.write) != 0 )
				return -1;
			p_socket->s_peer.write = NULL;
			break;
		case SHUTDOWN_BOTH:
			int r1 = pipe_writer_close(p_socket->s_peer.write);
			int r2 = pipe_reader_close(p_socket->s_peer.read);
			if( r1 == -1 || r2 == -1 )
				return -1;
			p_socket->s_peer.read  = NULL;
			p_socket->s_peer.write = NULL;
			break;

	}
	


	return 0;
}




// socket read/write/close

int socket_read(void* read, char* buf, uint size){

	SCB* scb = (SCB*)read;

	if (scb == NULL)
		return -1;

	if (scb->type != SOCKET_PEER || scb->s_peer.peer == NULL)
		return -1;

	int retval = pipe_read(scb->s_peer.read, buf, size);

	return retval;
}

int socket_write(void* write, const char* buf, uint size){

	SCB* scb = (SCB*)write;

	if(scb == NULL)
		return -1;

	if (scb->type != SOCKET_PEER || scb->s_peer.peer == NULL)
		return -1;

	int retval = pipe_write(scb->s_peer.write, buf, size);

	return retval;
}

int socket_close(void* fid){

	SCB* p_socket = (SCB*)fid;

	if (p_socket == NULL)
		return -1;

	if (p_socket->type == SOCKET_PEER) {
		if ( !(pipe_writer_close(p_socket->s_peer.write) || pipe_reader_close(p_socket->s_peer.read)) )
			return -1;
		p_socket->s_peer.peer = NULL;
	}

	if (p_socket->type == SOCKET_LISTENER) {

		PORTMAP[p_socket->port] = NULL;
		kernel_broadcast(&p_socket->s_listener.req_available);

	}


	p_socket->refcount--;

	if (!p_socket->refcount)
		free(p_socket);

	return 0;
}