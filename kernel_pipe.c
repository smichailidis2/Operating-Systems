
#include "tinyos.h"
#include "kernel_dev.h"
#include "kernel_streams.h"
#include "kernel_cc.h"


pipe_cb* p_PIPECB;


file_ops pipe_writer = {
	.Read = (void*)pipe_error,
	.Write = pipe_write,
	.Close = pipe_writer_close
};

file_ops pipe_reader = {
	.Read = pipe_read,
	.Write = (void*)pipe_error,
	.Close = pipe_reader_close
};


int sys_Pipe(pipe_t* pipe)
{	
	FCB* fcbs[2];  // array of pointers to reader[0] and writer[1] FCB
	Fid_t fids[2]; // array of file id of reader and writer

	int r = FCB_reserve(2,fids,fcbs);

	if (r == 0) {
		return -1; // error in FCB reservation
	}

	/*Create Pipe Control Block*/

	pipe_cb* p_PIPE_CB = (pipe_cb*)xmalloc(sizeof(pipe_cb));

	
	pipe->read = fids[0];
	pipe->write = fids[1];

	/*Initialize Pipe Control Block*/
	p_PIPE_CB->reader = fcbs[0];
	p_PIPE_CB->writer = fcbs[1];
	p_PIPE_CB->has_space = COND_INIT;
	p_PIPE_CB->has_data = COND_INIT;
	p_PIPE_CB->w_pos = 0;
	p_PIPE_CB->r_pos = 0;

	fcbs[0]->streamobj = p_PIPE_CB;
	fcbs[1]->streamobj = p_PIPE_CB;

	/*Initialize */
	fcbs[0]->streamfunc = &pipe_reader;
	fcbs[1]->streamfunc = &pipe_writer;



	return 0;
}

int pipe_write(void* pipecb_t, const char *buf, unsigned int n)
{
	return -1;
}

int pipe_read(void* pipecb_t, char *buf, unsigned int n)
{
	return -1;
}

int pipe_writer_close(void* _pipecb)
{
	return -1;
}

int pipe_reader_close(void* _pipecb)
{
	return -1;
}

//returns -1 
int pipe_error(void* pipecb_t, const char *buf, unsigned int n) 
{
	return -1;
}