/*
	Definition of P2PIOPolicy class

	Author: Benjamin Karl Bergen

	$Revision$
	$LastChangedBy$
	$LastChangedDate$
	vim: set ts=3 :
*/

#ifndef P2PIOPolicy_hxx
#define P2PIOPolicy_hxx

#include <cstdarg>
#include "FileIOData.hxx"
#include "P2PConnection.hxx"
#include "MPData.hxx"
#include "swap.h"

/*!
	\class P2PIOPolicy P2PIOPolicy.h
	\brief  provides...
*/
template<bool swapped>
class P2PIOPolicy
	{
	public:

		//! Constructor
		P2PIOPolicy()
			: current_(0), buffer_offset_(0), file_size_(0)
			{
				pending_[0] = false;
				pending_[1] = false;
				buffer_fill_[0] = 0;
				buffer_fill_[1] = 0;
			}

		//! Destructor
		~P2PIOPolicy() {}

		FileIOStatus open(const char * filename, FileIOMode mode);
		void close();

		void scan(const char * format, ...);
		void print(const char * format, ...);

		template<typename T> void read(T * data, size_t elements);
		template<typename T> void write(const T * data, size_t elements);

	private:

		template<typename T> inline void swap_bytes(T * data, size_t elements);

		void send_write_block(uint32_t buffer);
		void wait_write_block(uint32_t buffer);
		void flush();

		void request_read_block(uint32_t buffer);
		void wait_read_block(uint32_t buffer);

		MPBuffer<char, io_buffer_size> io_buffer_[2];
		MPBuffer<char, io_line_size> io_line_;

		uint32_t current_;
		uint64_t buffer_offset_;
		uint64_t buffer_fill_[2];
		bool pending_[2];
		int request_id[2];
		MPRequest request_[2];

		int file_size_;
		div_t read_blocks_;

	}; // class P2PIOPolicy

template<bool swapped>
FileIOStatus P2PIOPolicy<swapped>::open(const char * filename, FileIOMode mode)
	{
		P2PConnection & p2p = P2PConnection::instance();

		// this sends the string terminator as well
		size_t msg_size = strlen(filename)+1;
		MPRequest request;

		// re-initialize some values
		buffer_offset_ = 0;
		pending_[0] = false;
		pending_[1] = false;
		buffer_fill_[0] = 0;
		buffer_fill_[1] = 0;
		current_ = 0;

		// file io mode
		switch(mode) {

			case io_read:
				request.set(P2PTag::io_open_read, P2PTag::data, msg_size);
				p2p.post(request);
				break;

			case io_write:
				request.set(P2PTag::io_open_write, P2PTag::data, msg_size);
				p2p.post(request);
				break;

			case io_write_append:
				request.set(P2PTag::io_open_write_append,
					P2PTag::data, msg_size);
				p2p.post(request);
				break;

			default:
				return fail;

		} // switch

		// send the filename to peer
		p2p.send(const_cast<char *>(filename), request.count, request.tag);

		if(mode == io_read) {
			p2p.recv(&file_size_, 1, request.tag, request.id);
			read_blocks_ = div(file_size_, io_buffer_size);

			// request block
			request_read_block(current_);

			// request next block
			request_read_block(current_^1);

			// wait on the first block
			wait_read_block(current_);
		} // if

// FIXME: need to handle errors properly

		return ok;
	} // P2PIOPolicy<>::open

template<bool swapped>
void P2PIOPolicy<swapped>::close()
	{
		// force write if current block hasn't been written
		if(buffer_offset_ > 0) {
			flush();
		} // if

		P2PConnection::instance().post(P2PTag::io_close);
	} // P2PIOPolicy<>::close

template<bool swapped>
void P2PIOPolicy<swapped>::scan(const char * format, ...)
	{
		/*
		P2PConnection & p2p = P2PConnection::instance();

		MPRequest request(P2PTag::data, io_buffer_[current_].size(),
			current_);

		// request remote read
		p2p.post(P2PTag::io_read, request);
		p2p.recv(io_buffer_[current_].data(), request.count,
			request.tag, request.id);

		char * str = strtok(io_buffer_[current_].data(), "\n");

		va_list ab;
		va_start(ab, format);
		vsscanf(str, format, ab);
		str = strtok(NULL, "\n");
		*/
	} // P2PIOPolicy<>::scan

template<bool swapped>
void P2PIOPolicy<swapped>::print(const char * format, ...)
	{
		va_list ab;

		// initialize varg list
		va_start(ab, format);

		// sprintf to local buffer
		vsprintf(io_line_.data(), format, ab);

		// use write function to do actual work
		P2PIOPolicy::write(io_line_.data(), strlen(io_line_.data()));
	} // P2PIOPolicy<>::scan

template<bool swapped>
template<typename T>
void P2PIOPolicy<swapped>::read(T * data, size_t elements)
	{
		// everything is done in bytes
		uint64_t bytes = elements*sizeof(T);
		char * bdata = reinterpret_cast<char *>(data);
		uint64_t bdata_offset(0);

		do {
			const int64_t over_run = (buffer_offset_ + bytes) -
				buffer_fill_[current_];

			if(over_run > 0) {
				const uint64_t under_run =
					buffer_fill_[current_] - buffer_offset_;

				// copy remainder of current buffer to data
				memcpy(bdata + bdata_offset,
					io_buffer_[current_].data() + buffer_offset_, under_run);
				bdata_offset += under_run;
				bytes -= under_run;

				// re-fill current buffer
				request_read_block(current_);
				current_^=1;
				if(pending_[current_]) {
					wait_read_block(current_);
				} // if
				buffer_offset_ = 0;
			}
			else {
				memcpy(bdata + bdata_offset,
					io_buffer_[current_].data() + buffer_offset_, bytes);
				buffer_offset_ += bytes;
				bytes = 0;
			} // if
		} while(bytes > 0);

		// this will only do something if
		// this class was instantiated as P2PIOPolicy<true>
		swap_bytes(data, elements);
	} // P2PIOPolicy<>::read

template<bool swapped>
template<typename T>
void P2PIOPolicy<swapped>::write(const T * data, size_t elements)
	{
		// book-keeping is done in bytes
		uint64_t bytes(elements*sizeof(T));
		const char * bdata = reinterpret_cast<const char *>(data);
		uint64_t bdata_offset(0);

		do {
			const int64_t over_run = (buffer_offset_ + bytes) -
				io_buffer_[current_].size();

			if(over_run > 0) {
				const uint64_t under_run =
					io_buffer_[current_].size() - buffer_offset_;

				// because of the possiblity of byte swapping
				// we need to make sure that only even multiples
				// of the type are copied at once
				const uint64_t copy_bytes = (under_run/sizeof(T))*sizeof(T);

				memcpy(io_buffer_[current_].data() + buffer_offset_,
					bdata + bdata_offset, copy_bytes);

				// need to force type here to get correct swapping
				swap_bytes<T>(reinterpret_cast<T *>
					(io_buffer_[current_].data() + buffer_offset_),
					copy_bytes/sizeof(T));

				bdata_offset += copy_bytes;
				bytes -= copy_bytes;

				request_[current_].set(P2PTag::io_write, P2PTag::data,
					buffer_offset_ + copy_bytes, current_);
				send_write_block(current_);
				current_^=1;
				if(pending_[current_]) {
					wait_write_block(current_);
				} // if
				buffer_offset_ = 0;
			}
			else {
				memcpy(io_buffer_[current_].data() + buffer_offset_,
					bdata + bdata_offset, bytes);

				// need to force type here to get correct swapping
				swap_bytes(reinterpret_cast<T *>
					(io_buffer_[current_].data() + buffer_offset_),
					bytes/sizeof(T));

				buffer_offset_ += bytes;
				bytes = 0;
			} // if
		} while(bytes > 0);
	} // P2PIOPolicy<>::write

template<bool swapped>
void P2PIOPolicy<swapped>::request_read_block(uint32_t buffer)
	{
		P2PConnection & p2p = P2PConnection::instance();

		if(read_blocks_.quot > 0) {
			request_[buffer].set(P2PTag::io_read, P2PTag::data,
				io_buffer_[buffer].size(), buffer);
			p2p.post(request_[buffer]);
			p2p.irecv(io_buffer_[buffer].data(), request_[buffer].count,
				request_[buffer].tag, request_[buffer].id);
			pending_[buffer] = true;
			buffer_fill_[buffer] = request_[buffer].count;

			read_blocks_.quot--;
		}
		else if(read_blocks_.rem > 0) {
			request_[buffer].set(P2PTag::io_read, P2PTag::data,
				read_blocks_.rem, buffer);
			p2p.post(request_[buffer]);
			p2p.irecv(io_buffer_[buffer].data(), request_[buffer].count,
				request_[buffer].tag, request_[buffer].id);
			pending_[buffer] = true;
			buffer_fill_[buffer] = request_[buffer].count;

			read_blocks_.rem = 0;
		} // if
	} // P2PIOPolicy<>::request_read_block

template<bool swapped>
void P2PIOPolicy<swapped>::wait_read_block(uint32_t buffer)
	{
		P2PConnection & p2p = P2PConnection::instance();
		p2p.wait_recv(request_[buffer].id);
		pending_[buffer] = false;
	} // P2PIOPolicy<>::wait_read_block

template<bool swapped>
void P2PIOPolicy<swapped>::send_write_block(uint32_t buffer)
	{
		P2PConnection & p2p = P2PConnection::instance();

		p2p.post(request_[buffer]);
		p2p.isend(io_buffer_[buffer].data(), request_[buffer].count,
			request_[buffer].tag, request_[buffer].id);
		pending_[buffer] = true;
	} // P2PIOPolicy<>::send_write_block

template<bool swapped>
void P2PIOPolicy<swapped>::wait_write_block(uint32_t buffer)
	{
		P2PConnection & p2p = P2PConnection::instance();
		p2p.wait_send(request_[buffer].id);
		pending_[buffer] = false;
	} // P2PIOPolicy<>::wait_write_block

template<bool swapped>
void P2PIOPolicy<swapped>::flush()
	{
		// request remote write
		request_[current_].set(P2PTag::io_write, P2PTag::data,
			buffer_offset_, current_);
		send_write_block(current_);
		current_ ^= 1;

		if(pending_[current_]) {
			wait_write_block(current_);
		} // if
	} // P2PIOPolicy<>::flush

template<>
template<typename T> inline
void P2PIOPolicy<true>::swap_bytes(T * data, size_t elements)
	{
		for(size_t i(0); i<elements; i++) {
			utils::swap(data[i]);
		} // for
	} // P2PIOPolicy<>::swap_bytes

template<>
template<typename T> inline
void P2PIOPolicy<false>::swap_bytes(T * data, size_t elements)
	{
	} // P2PIOPolicy<>::swap_bytes

#endif // P2PIOPolicy_hxx
