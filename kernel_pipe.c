
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
	p_PIPE_CB->available_buffer_space = PIPE_BUFFER_SIZE;

	fcbs[0]->streamobj = p_PIPE_CB;
	fcbs[1]->streamobj = p_PIPE_CB;

	/*Initialize */
	fcbs[0]->streamfunc = &pipe_reader;
	fcbs[1]->streamfunc = &pipe_writer;



	return 0;
}

int pipe_write(void* pipecb_t, const char *buf, unsigned int n)
{
	pipe_cb* p_pipe= (pipe_cb*)pipecb_t;


	// check if pipe or the in/out streamfunctions are NULL
	if (p_pipe == NULL || p_pipe->reader == NULL || p_pipe->writer == NULL) {
		return -1;
	}

	uint available_bytes = p_pipe->available_buffer_space;
	

	/*------- ENTER IN CRITICAL SECTION -------*/
	while(available_bytes == 0 && p_pipe->reader != NULL) {
		// while there is no room to write , we must signal the reader , and then wait for it to read some data.
		kernel_wait(&p_pipe->has_space, SCHED_PIPE);

		// When writer resurrects, the w_pos and r_pos will have changed, so available bytes needs to be re-evaluated
		available_bytes = p_pipe->available_buffer_space;
	}



	//if the available bytes are less than the length of buffer to be copied, only the available bytes will be filled.
	uint k = (n > available_bytes ) ? available_bytes : n ;

	/*=== PERFORM WRITE OPERATION ===*/
	for (int i=0; i < k; i++) {
		p_pipe->BUFFER[p_pipe->w_pos++] = buf[i];
		p_pipe->w_pos %= PIPE_BUFFER_SIZE; //cyclic buffer : new w_pos is (w_pos+1)mod(PIPE_BUFFER_SIZE).
		p_pipe->available_buffer_space--;
	}

	// Resurrect all readers
	kernel_broadcast(&p_pipe->has_data);

	return k;
}




int pipe_read(void* pipecb_t, char *buf, unsigned int n)
{
	pipe_cb* p_pipe= (pipe_cb*)pipecb_t;


	// check if pipe or the in/out streamfunctions are NULL
	if (p_pipe == NULL || p_pipe->reader == NULL) {
		return -1;
	}

	uint available_bytes = p_pipe->available_buffer_space;

	// if there is no writer and pipe buffer is empty, bytes read = 0.
	if (p_pipe->writer == NULL && available_bytes == PIPE_BUFFER_SIZE)
		return 0;


	/*------- ENTER IN CRITICAL SECTION -------*/
	while( available_bytes == PIPE_BUFFER_SIZE ) {
		// while there are no data written , we must wait until writer writes some data.
		kernel_wait(&p_pipe->has_data, SCHED_PIPE);

		// When reader resurrects, the w_pos and r_pos will have changed, so available bytes needs to be re-evaluated
		available_bytes = p_pipe->available_buffer_space;
	}

	uint bytes_to_read = PIPE_BUFFER_SIZE - available_bytes;

	// if size of buffer n is less than bytes to be read, read only n chars.
	uint k = (n < bytes_to_read) ? (n) : (bytes_to_read) ;

	/*=== PERFORM READ OPERATION ===*/
	for (int i=0; i < k; i++) {
		buf[i] = p_pipe->BUFFER[p_pipe->r_pos++];
		p_pipe->r_pos %= PIPE_BUFFER_SIZE; //cyclic buffer : new r_pos is (r_pos+1)mod(PIPE_BUFFER_SIZE).
		p_pipe->available_buffer_space++;
	}

	// resurrect all readers
	kernel_broadcast(&p_pipe->has_space);

	return k;
}





int pipe_writer_close(void* _pipecb)
{
	pipe_cb* p_pipe = (pipe_cb*) _pipecb;

	if (p_pipe == NULL)
		return -1;

	p_pipe->writer = NULL;

	// if there is no reader fcb we free all the pipe I/O and the pipe itself.
	if (p_pipe->reader == NULL) {
		free(p_pipe->writer);
		free(p_pipe->reader);
		free(p_pipe);
	}


	return 0;
}





int pipe_reader_close(void* _pipecb)
{

	pipe_cb* p_pipe = (pipe_cb*)_pipecb;

	if (p_pipe == NULL)
		return -1;

	p_pipe->reader = NULL;

	uint bytes_to_read = PIPE_BUFFER_SIZE - p_pipe->available_buffer_space;

	// if there are no available data to read and there is no writer , free all
	if (bytes_to_read == 0 && p_pipe->writer == NULL) {
		free(p_pipe->reader);
		free(p_pipe->writer);
		free(p_pipe);
	}


	return 0;
}





//returns -1 always
int pipe_error(void* pipecb_t, const char *buf, unsigned int n) 
{
	return -1;
}