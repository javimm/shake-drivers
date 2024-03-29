/*	Copyright (c) 2006-2009, University of Glasgow
*	All rights reserved.
*
*	Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:
*
*		* Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
*		* Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer
*			in the documentation and/or other materials provided with the distribution.
*		* Neither the name of the University of Glasgow nor the names of its contributors may be used to endorse or promote products derived 
*			from this software without specific prior written permission.
*
*	THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, 
*	THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS 
*	BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE 
*	GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT 
*	LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/


#include "SK6.h"
#include "SK6_packets.h"
#include "SK6_parsing.h"
#include "shake_parsing.h"

SK6::SK6(shake_device* sd, shake_device_private* sdp) : SHAKE(sd, sdp)
{
}

SK6::~SK6(void)
{
}

// parses a complete ASCII packet
int SK6::parse_ascii_packet(int packet_type, char* packetbuf, int packetlen, int playback, void* timestamp_packet) {
	if(packet_type != SK6_ACK_ACK && packet_type != SK6_ACK_NEG) {
		if(devpriv->checksum) {
			SHAKE_DBG("^^^ Parsing ASCII+checksum\n");
			extract_ascii_packet(packet_type, packetbuf, playback, timestamp_packet);
		} else {
			SHAKE_DBG("### Parsing ASCII (no checksum)\n");
			extract_ascii_packet(packet_type, packetbuf, playback, timestamp_packet);
		}
	} else {
		if(!devpriv->waiting_for_ack) {
			SHAKE_DBG("WARNING: SKIPPED ACK: %s", packetbuf);
			devpriv->waiting_for_ack_signal = FALSE;
			return SK6_ASCII_READ_ERROR;
		}

		/* ack packet, check type, then extract the address and value from it into the devpriv structure, 
		* where it can be retrieved by the thread which sent the initial command packet */
		if(packet_type == SK6_ACK_ACK)
			devpriv->lastack = TRUE;
		else
			devpriv->lastack = FALSE;
		
		parse_ack_packet(packetbuf, devpriv->lastaddr, devpriv->lastval);

		devpriv->waiting_for_ack_signal = FALSE;
		SHAKE_DBG("ACK signalled\n");
	}

	return SK6_ASCII_READ_OK;
}

// reads a complete ASCII packet (minus header)
int SK6::read_ascii_packet(int packet_type, char* packetbuf) {
	int packet_size = 0, bytes_left, bytes_read = 0;
	BOOL playback = FALSE;
	sk6_data_timestamp_packet timestamp_pkt;

	/* check if this is a $TIM packet */
	if(packet_type == SK6_DATA_TIMESTAMP) {
		// read up to end of the packet prefix (before the actual data packet)
		read_bytes(devpriv, packetbuf + SK6_HEADER_LEN, sk6_packet_lengths[SK6_DATA_TIMESTAMP] - SK6_HEADER_LEN);
		// copy packet so far into the timestamp packet structure
		memcpy(&(timestamp_pkt), packetbuf, sk6_packet_lengths[SK6_DATA_TIMESTAMP]);

		playback = TRUE;

		// now read the encapsulated data packet header, and continue normally
		memset(packetbuf, 0, 256);
		read_bytes(devpriv, packetbuf, SK6_HEADER_LEN);
		packet_type = classify_packet_header(packetbuf, SK6_HEADER_LEN, TRUE);
	} 
	/* check if playback complete */
	else if (packet_type == SK6_DATA_PLAYBACK_COMPLETE) {
		// read remainder of the packet
		read_bytes(devpriv, packetbuf + SK6_HEADER_LEN, sk6_packet_lengths[packet_type] - SK6_HEADER_LEN);
		playback = FALSE;

		// if event callback registered, signal that playback is completed
		if(devpriv->navcb || devpriv->navcb_STDCALL) {
			devpriv->lastevent = SHAKE_PLAYBACK_COMPLETE;
			shake_thread_signal(&(devpriv->thread), CALLBACK_THREAD);
		}
		return SK6_ASCII_READ_CONTINUE;
	} else if (packet_type == SK6_DATA_RFID_TID) {
		SHAKE_DBG("RFID TAG FOUND\n");
		// read rest of the packet
		read_bytes(devpriv, packetbuf + SK6_HEADER_LEN, sk6_packet_lengths[packet_type] - SK6_HEADER_LEN);
		// copy to buffer
		memcpy(devpriv->lastrfid, packetbuf + SK6_HEADER_LEN + 1, SHAKE_RFID_TAG_LENGTH);

		// if event callback registered, signal the event
		if(devpriv->navcb || devpriv->navcb_STDCALL) {
			devpriv->lastevent = SHAKE_RFID_TID_EVENT;
			shake_thread_signal(&(devpriv->thread), CALLBACK_THREAD);
		}
		return SK6_ASCII_READ_CONTINUE;
	} else if(packet_type == SK6_STARTUP_INFO) {
		read_device_info();
		return SK6_ASCII_READ_CONTINUE;
	}

	SHAKE_DBG("+++ Calculating bytes left\n");
	/* read up to the non-checksummed length of the packet */
	bytes_left = sk6_packet_lengths[packet_type] - SK6_HEADER_LEN;
	SHAKE_DBG("+++ %d bytes left to read\n", bytes_left);

	/*  all data packets which normally have the 3 byte sequence number section in them 
	*	so subtract 3 bytes from expected length if in playback mode. A dummy sequence number is
	*	inserted below to make parsing a bit simpler. */
	if(playback)
		bytes_left -= 3;

	if((bytes_read = read_bytes(devpriv, packetbuf + SK6_HEADER_LEN, bytes_left)) != bytes_left) {
		SHAKE_DBG("Failed to get full packet (%d/%d))\n", bytes_read, bytes_left);
		return SK6_ASCII_READ_ERROR;
	}
	
	SHAKE_DBG("Got full packet OK (%d/%d)\n", bytes_read, bytes_left);
	if(playback) {
		// want to change packetbuf so that the 2 bytes currently at the end of the packet "\r\n" instead
		// become ",00\r\n"...
		int offset = bytes_read + SK6_HEADER_LEN - 2;
		packetbuf[offset] = ',';
		packetbuf[offset+1] = '0';
		packetbuf[offset+2] = '0';
		packetbuf[offset+3] = '\r';
		packetbuf[offset+4] = '\n';
		bytes_read += 3;
	}

	/* if the packet is of a type that can have a checksum, and the last character in the current
	*	packet buffer is not the \n terminator, it means checksums are now enabled */
	if(sk6_packet_has_checksum[packet_type] && packetbuf[bytes_read + SK6_HEADER_LEN - 1] != 0xA) {
		if(devpriv && !devpriv->checksum) {
			devpriv->checksum = TRUE;
			SHAKE_DBG("CHECKSUMMING NOW ON!\n");
		}
		/* read the final 3 bytes of the packet */
		read_bytes(devpriv, packetbuf + SK6_HEADER_LEN + bytes_read, CHECKSUM_LENGTH);
		bytes_read += 3;
	/* if the packet is of a type that can have a checksum, and the last character IS the
	*	\n terminator, it means checksums are now disabled, so record that */
	} else if (sk6_packet_has_checksum[packet_type] && packetbuf[bytes_read + SK6_HEADER_LEN - 1] == 0xA && devpriv->checksum) {
		if(devpriv)
			devpriv->checksum = FALSE;
		SHAKE_DBG("CHECKSUMMING NOW OFF!\n");
	}

	SHAKE_DBG("ASCII type %d complete\n", packet_type);

	packet_size = bytes_read + SK6_HEADER_LEN;

	if(devpriv->rthread_done) 
		return SK6_ASCII_READ_ERROR;
	return parse_ascii_packet(packet_type, packetbuf, packet_size, playback, &timestamp_pkt);
}

// parses a complete raw packet
int SK6::parse_raw_packet(int packet_type, char* packetbuf, int packetlen, int has_seq) {
	SHAKE_DBG("*** Parsing raw\n");
	extract_raw_packet(packet_type, packetbuf, has_seq);
	// check if we need to send an audio packet back
	if(packet_type == SK6_RAW_DATA_AUDIO_HEADER) {
		shake_compress_and_send_audio(devpriv);
	}
	return SK6_RAW_READ_OK;
}

// reads a complete raw packet (minus header)
int SK6::read_raw_packet(int packet_type, char* packetbuf) {
	int packet_size = 0, bytes_left, bytes_read = 0;

	/* calculate bytes remaining and read them */
	bytes_left = sk6_packet_lengths[packet_type] - SK6_RAW_HEADER_LEN;
	bytes_read = read_bytes(devpriv, packetbuf + SK6_RAW_HEADER_LEN, bytes_left);
	SHAKE_DBG("bytes_left = %d, bytes_read = %d\n", bytes_left, bytes_read);

	if(bytes_left - bytes_read > 1) {
		SHAKE_DBG("ERROR READING RAW PACKET #2\n");
		SHAKE_DBG("%02X %02X %02X\n", packetbuf[0], packetbuf[1], packetbuf[2]);
		return SK6_RAW_READ_ERROR;
	}

	// raw packets now have optional sequence number, check for it here
	BOOL has_seq = FALSE;
	devpriv->peek_flag = FALSE;

	SHAKE_DBG("Checking trailing byte\n");

	// if we got a full packet, last byte may or may not be a sequence number
	if(bytes_left == bytes_read) {
		int packet_len = sk6_packet_lengths[packet_type];
		char trailing_byte = packetbuf[packet_len-1];

		if(trailing_byte == 0x7F) {
			// 0x7F is ALWAYS a header value for a raw data packet
			devpriv->peek = 0x7F;
			devpriv->peek_flag = TRUE;
			SHAKE_DBG("Setting peek flag\n");
		} else if(trailing_byte == '$' || trailing_byte == '\n') {
			// $ or \n could either be a sequence number byte, OR the first byte in an ASCII
			// data packet following this one
			// no way to be 100% sure but can be relatively certain by checking the current sequence number
			// for the current packet type. if it is not equal to the value of the byte + 1, very likely
			// that it's not a sequence number. otherwise treat it as a sequence number

			// adjust packet type to index into the array properly
			int adjtype = packet_type - SK6_RAW_DATA_ACC;

			if(trailing_byte == data.internal_timestamps[adjtype] + 1 
				|| (trailing_byte == 0 && data.internal_timestamps[adjtype] == 255)) {
				
				// treat this as a sequence number byte
				has_seq = TRUE;
				SHAKE_DBG("Sequence number found\n");
			} else {
				SHAKE_DBG(">>> Preserving a trailing byte for an ASCII packet: %02X\n", trailing_byte);
				// preserve the byte for next packet
				devpriv->peek_flag = TRUE;
				devpriv->peek = trailing_byte;
			}
		} else {
			// treat this as a sequence number byte
			has_seq = TRUE;
			SHAKE_DBG("Sequence number found\n");
		}
	} else {
		// no trailing byte means sequence numbers are off (probably)
		SHAKE_DBG("No trailing byte\n");
	}

	return parse_raw_packet(packet_type, packetbuf, bytes_read + SK6_RAW_HEADER_LEN, has_seq);
}

// finds and classifies next packet header in the data stream
int SK6::get_next_packet(char* packetbuf, int bufsize) {
	int bytes_read = 0, packet_type = SHAKE_BAD_PACKET;

	/*	start by reading 3 bytes, since raw headers are 3 bytes while ASCII headers are 4 bytes
	*	the smaller of the two is used */
	bytes_read = read_bytes(devpriv, packetbuf, 3);
	SHAKE_DBG("ML) Read initial header: %d bytes\n", bytes_read);

	/* only attempt to classify the header if all 3 bytes were read successfully */
	if(bytes_read == 3) {
		/* check if the first two bytes are 0x7F 0x7F, indicating a raw packet header */
		if(packetbuf[0] == 0x7F && packetbuf[1] == 0x7F) {
			SHAKE_DBG("ML) Found raw header, classifying...\n");
			packet_type = classify_packet_header(packetbuf, SK6_RAW_HEADER_LEN, FALSE);
			SHAKE_DBG("ML) Type = %d (%s)\n", packet_type, sk6_packet_type_names[packet_type]);
		/* check if the first byte is a $ character, indicating an ASCII header 
			also special case of SHAKE startup splash, \n character */
		} else if(packetbuf[0] == '$' || packetbuf[0] == '\n') {
			/* read an extra byte to complete the header before classifying */
			SHAKE_DBG("ML) ASCII header/SHAKE info found, reading 4th byte\n");
			read_bytes(devpriv, packetbuf+3, 1);
			packet_type = classify_packet_header(packetbuf, SK6_HEADER_LEN, TRUE);
			SHAKE_DBG("ML) ASCII Type = %d (%s)\n", packet_type, sk6_packet_type_names[packet_type]);
		}
	}

	/* if packet remains unclassified, try to find the next header in the data stream */
	if(packet_type == SHAKE_BAD_PACKET) {
		int i;
		char c = ' ';

		SHAKE_DBG("SHAKE_BAD_PKT\n");
		SHAKE_DBG("packetbuf: \"%s\"\n", packetbuf);
		SHAKE_DBG("ML) packetbuf bin:");
		for(i=0;i<4;i++) {
			SHAKE_DBG("%d/%02X ", packetbuf[i], packetbuf[i]);
		}
		SHAKE_DBG("\n");
		memset(packetbuf, 0, 256);

		int read_count = 0;
		/* read up to 50 bytes until next header is found. maximum SHAKE packet size is less than 
		*	40 bytes so normally this should guarantee that a new header is found.
		*	the function just checks for $ and 0x7F characters, indicating the start of an
		*	ASCII or raw header respectively */
		while(read_count++ < 50 && (c != '$' && c != 0x7F)) {
			read_bytes(devpriv, &c, 1);
		}
		packetbuf[0] = c;
		
		/* check for a possible ASCII header and attempt to classify */
		if(c == '$') {
			SHAKE_DBG("ML-EH) Found ASCII header\n");
			read_bytes(devpriv, packetbuf+1, SK6_HEADER_LEN-1);
			packet_type = classify_packet_header(packetbuf, SK6_HEADER_LEN, TRUE);
			SHAKE_DBG("ML-EH) ASCII Type = %d (%s)\n", packet_type, sk6_packet_type_names[packet_type]);
		/* check for a possible RAW header and attempt to classify */
		} else if(c == 0x7F) {
			SHAKE_DBG("ML-EH) Found raw header\n");
			read_bytes(devpriv, packetbuf+1, SK6_RAW_HEADER_LEN-1);
			packet_type = classify_packet_header(packetbuf, SK6_RAW_HEADER_LEN, FALSE);
			SHAKE_DBG("ML-EH) Type = %d (%s)\n", packet_type, sk6_packet_type_names[packet_type]);
		}
	}

	return packet_type;
}

// parses a complete packet with an identified type
int SK6::parse_packet(char* packetbuf, int packet_type) {
	/* deal with the packet separately depending on whether it is ASCII or RAW */
	if(is_ascii_packet(packet_type)) {
		devpriv->rthread_exit = 5;
		SHAKE_DBG("ML) parsing ASCII packet\n");
		read_ascii_packet(packet_type, packetbuf);
		devpriv->rthread_exit = 6;
	} else {
		devpriv->rthread_exit = 7;
		SHAKE_DBG("ML) parsing raw packet\n");
		read_raw_packet(packet_type, packetbuf);
		devpriv->rthread_exit = 8;
	}

	return 1;
}

int SK6::is_ascii_packet(int packet_type) {
	if(packet_type >= SK6_DATA_ACC && packet_type < SK6_RAW_DATA_ACC) 
		return 1;
	return 0;
}

int SK6::extract_ascii_packet(int packet_type, char* rawpacket, int playback, void* timestamp_packet) {
	int ev = -1;
	sk6_data_timestamp_packet* timestamp = (sk6_data_timestamp_packet*)timestamp_packet;

	switch(packet_type) {
		case SK6_DATA_ACC: {
			int seq;
			sk6_data_acc_packet* dataacc = (sk6_data_acc_packet*)rawpacket;
			data.accx = dec_ascii_to_int(dataacc->accx.data, 5, 4);
			data.accy = dec_ascii_to_int(dataacc->accy.data, 5, 4);
			data.accz = dec_ascii_to_int(dataacc->accz.data, 5, 4);
			seq = dec_ascii_to_int(dataacc->seq.data, 2, 2);
			data.internal_timestamps[SHAKE_SENSOR_ACC] = seq;

			if(playback && devpriv->log) {
				double tsval = dec_ascii_to_int(timestamp->timestamp, 10, 10) / 100.0;
				fprintf(devpriv->log, "%.3f,ACC,%d,%d,%d,%d\n", tsval, SHAKE_SENSOR_ACC, data.accx, data.accy, data.accz);
			}
			break;
		}
		case SK6_DATA_GYRO: {
			sk6_data_gyr_packet* datagyr = (sk6_data_gyr_packet*)rawpacket;
			int seq;
			data.gyrx = dec_ascii_to_int(datagyr->gyrx.data, 5, 4);
			data.gyry = dec_ascii_to_int(datagyr->gyry.data, 5, 4);
			data.gyrz = dec_ascii_to_int(datagyr->gyrz.data, 5, 4);
			seq = dec_ascii_to_int(datagyr->seq.data, 2, 2);
			data.internal_timestamps[SHAKE_SENSOR_GYRO] = seq;

			if(playback && devpriv->log) {
				double tsval = dec_ascii_to_int(timestamp->timestamp, 10, 10) / 100.0;
				fprintf(devpriv->log, "%.3f,GYR,%d,%d,%d,%d\n", tsval, SHAKE_SENSOR_GYRO, data.gyrx, data.gyry, data.gyrz);
			}
			break;
		}
		case SK6_DATA_MAG: {
			sk6_data_mag_packet* datamag = (sk6_data_mag_packet*)rawpacket;
			int seq;
			data.magx = dec_ascii_to_int(datamag->magx.data, 5, 4);
			data.magy = dec_ascii_to_int(datamag->magy.data, 5, 4);
			data.magz = dec_ascii_to_int(datamag->magz.data, 5, 4);
			seq = dec_ascii_to_int(datamag->seq.data, 2, 2);
			data.internal_timestamps[SHAKE_SENSOR_MAG] = seq;

			if(playback && devpriv->log) {
				double tsval = dec_ascii_to_int(timestamp->timestamp, 10, 10) / 100.0;
				fprintf(devpriv->log, "%.3f,MAG,%d,%d,%d,%d\n", tsval, SHAKE_SENSOR_MAG, data.magx, data.magy, data.magz);
			}
			break;
		}
		case SK6_DATA_HEADING: {
			sk6_data_heading_packet* datahdg = (sk6_data_heading_packet*)rawpacket;
			int seq;
			data.heading = dec_ascii_to_int(datahdg->heading.data, 4, 4);
			seq = dec_ascii_to_int(datahdg->seq.data, 2, 2);
			data.internal_timestamps[SHAKE_SENSOR_HEADING] = seq;

			if(playback && devpriv->log) {
				double tsval = dec_ascii_to_int(timestamp->timestamp, 10, 10) / 100.0;
				fprintf(devpriv->log, "%.3f,HED,%d,%d\n", tsval, SHAKE_SENSOR_HEADING, data.heading);
			}
			break;
		}
		case SK6_DATA_CAP0: {
			sk6_data_cap_packet* datacap = (sk6_data_cap_packet*)rawpacket;
			int seq;
			data.cap_sk6[0] = dec_ascii_to_int(datacap->prox.data, 4, 4);
			seq = dec_ascii_to_int(datacap->seq.data, 2, 2);
			data.internal_timestamps[SHAKE_SENSOR_SK6_CAP0] = seq;

			if(playback && devpriv->log) {
				double tsval = dec_ascii_to_int(timestamp->timestamp, 10, 10) / 100.0;
				fprintf(devpriv->log, "%.3f,CS0,%d,%d\n", tsval, SHAKE_SENSOR_SK6_CAP0, data.cap_sk6[0]);
			}
			break;
		}
		case SK6_DATA_CAP1: {
			sk6_data_cap_packet* datacap = (sk6_data_cap_packet*)rawpacket;
			int seq;
			data.cap_sk6[1] = dec_ascii_to_int(datacap->prox.data, 4, 4);		
			seq = dec_ascii_to_int(datacap->seq.data, 2, 2);
			data.internal_timestamps[SHAKE_SENSOR_SK6_CAP1] = seq;
	
			if(playback && devpriv->log) {
				double tsval = dec_ascii_to_int(timestamp->timestamp, 10, 10) / 100.0;
				fprintf(devpriv->log, "%.3f,CS1,%d,%d\n", tsval, SHAKE_SENSOR_SK6_CAP1, data.cap_sk6[1]);
			}
			break;
		}
		case SK6_DATA_ANA0: {
			sk6_data_analog_packet* dataana = (sk6_data_analog_packet*)rawpacket;
			int seq;
			data.ana0 = dec_ascii_to_int(dataana->voltage.data, 4, 4);
			seq = dec_ascii_to_int(dataana->seq.data, 2, 2);
			data.internal_timestamps[SHAKE_SENSOR_ANA0] = seq;
			
			if(playback && devpriv->log) {
				double tsval = dec_ascii_to_int(timestamp->timestamp, 10, 10) / 100.0;
				fprintf(devpriv->log, "%.3f,AI0,%d,%d\n", tsval, SHAKE_SENSOR_ANA0, data.ana0);
			}
			break;
		}
		case SK6_DATA_ANA1: {
			sk6_data_analog_packet* dataana = (sk6_data_analog_packet*)rawpacket;
			int seq;
			data.ana1 = dec_ascii_to_int(dataana->voltage.data, 4, 4);
			seq = dec_ascii_to_int(dataana->seq.data, 2, 2);
			data.internal_timestamps[SHAKE_SENSOR_ANA1] = seq;

			if(playback && devpriv->log) {
				double tsval = dec_ascii_to_int(timestamp->timestamp, 10, 10) / 100.0;
				fprintf(devpriv->log, "%.3f,AI1,%d,%d\n", tsval, SHAKE_SENSOR_ANA1, data.ana1);
			}
			break;
		}
		case SK6_DATA_NVU: case SK6_DATA_NVD:
		case SK6_DATA_NVC: case SK6_DATA_NVN:
			if(devpriv->navcb || devpriv->navcb_STDCALL) {
				int type;
				sk6_nav_packet* snp = (sk6_nav_packet*)rawpacket;
				
				type = -1;
				switch(snp->hdr.packet_id[2]) {
					case 'U':
						type = SHAKE_NAV_UP;
						break;
					case 'D':
						type = SHAKE_NAV_DOWN;
						break;
					case 'C':
						type = SHAKE_NAV_CENTRE;
						break;
					case 'N':
						type = SHAKE_NAV_NORMAL;
						break;
					default:
						type = -1;
						break;
				}
				devpriv->lastevent = type;
				shake_thread_signal(&(devpriv->thread), CALLBACK_THREAD);
			}
			break;
		case SK6_DATA_CU0:
			ev = SK6_CS0_UPPER;
		case SK6_DATA_CL0:
			ev = SK6_CS0_LOWER;
		case SK6_DATA_CU1: 
			ev = SK6_CS1_UPPER;
		case SK6_DATA_CL1:
			ev = SK6_CS1_LOWER;
			if(devpriv->navcb || devpriv->navcb_STDCALL) {
				devpriv->lastevent = ev;
				shake_thread_signal(&(devpriv->thread), CALLBACK_THREAD);
			}
			break;
		case SK6_DATA_SHAKING: {
			sk6_data_shake_packet* datashaking = (sk6_data_shake_packet*)rawpacket;
			int seq;
			data.shaking_peak_accel = dec_ascii_to_int(datashaking->peakaccel.data, 5, 4);
			data.shaking_direction = dec_ascii_to_int(datashaking->direction.data, 5, 4);
			data.shaking_timestamp = dec_ascii_to_int(datashaking->timestamp.data, 5, 4);
			seq = dec_ascii_to_int(datashaking->seq.data, 2, 2);
			data.sk6seq = seq;
		
			if(devpriv->navcb || devpriv->navcb_STDCALL) {
				devpriv->lastevent = SHAKE_SHAKING_EVENT;
				shake_thread_signal(&(devpriv->thread), CALLBACK_THREAD);
			}
			break;
		}
		case SK6_DATA_HEART_RATE: {
			sk6_data_hr_packet* datahr = (sk6_data_hr_packet*)rawpacket;
			int seq;
			data.hr_bpm = dec_ascii_to_int(datahr->heart_bpm.data, 4, 4);
			seq = dec_ascii_to_int(datahr->seq.data, 2, 2);
			data.hrseq = seq;

			if(devpriv->navcb || devpriv->navcb_STDCALL) {
				devpriv->lastevent = SHAKE_HEART_RATE_EVENT;
				shake_thread_signal(&(devpriv->thread), CALLBACK_THREAD);
			}
		}
		default:
			return SHAKE_ERROR;
	}

	if(playback)
		devpriv->packets_read++;

	return SHAKE_SUCCESS;
}

int SK6::extract_raw_packet(int packet_type, char* rawpacket, int has_seq) {
	sk6_raw_packet_long* srpl;
	sk6_raw_packet_short* srps;
	sk6_raw_packet_audio* saud;
	int ev;
	int len = sk6_packet_lengths[packet_type];

	switch(packet_type) {
		case SK6_RAW_DATA_ACC:
			srpl = (sk6_raw_packet_long*)rawpacket;
			data.accx = srpl->data[0] + (srpl->data[1] << 8);
			data.accy = srpl->data[2] + (srpl->data[3] << 8);
			data.accz = srpl->data[4] + (srpl->data[5] << 8);
			if(has_seq) data.internal_timestamps[SHAKE_SENSOR_ACC] = srpl->seq;
			
			break;
		case SK6_RAW_DATA_GYRO:
			srpl = (sk6_raw_packet_long*)rawpacket;
			data.gyrx = srpl->data[0] + (srpl->data[1] << 8);
			data.gyry = srpl->data[2] + (srpl->data[3] << 8);
			data.gyrz = srpl->data[4] + (srpl->data[5] << 8);
			if(has_seq) data.internal_timestamps[SHAKE_SENSOR_GYRO] = srpl->seq;
			break;
		case SK6_RAW_DATA_MAG:
			srpl = (sk6_raw_packet_long*)rawpacket;
			data.magx = srpl->data[0] + (srpl->data[1] << 8);
			data.magy = srpl->data[2] + (srpl->data[3] << 8);
			data.magz = srpl->data[4] + (srpl->data[5] << 8);
			if(has_seq) data.internal_timestamps[SHAKE_SENSOR_MAG] = srpl->seq;
			break;
		case SK6_RAW_DATA_HEADING:
			srps = (sk6_raw_packet_short*)rawpacket;
			data.heading = srps->data[0] + (srps->data[1] << 8);
			if(has_seq) data.internal_timestamps[SHAKE_SENSOR_HEADING] = srps->seq;
			break;
		case SK6_RAW_DATA_CAP0:
			srps = (sk6_raw_packet_short*)rawpacket;
			data.cap_sk6[0] = srps->data[0] + (srps->data[1] << 8);
			if(has_seq) data.internal_timestamps[SHAKE_SENSOR_SK6_CAP0] = srps->seq;
			break;
		case SK6_RAW_DATA_CAP1:
			srps = (sk6_raw_packet_short*)rawpacket;
			data.cap_sk6[1] = srps->data[0] + (srps->data[1] << 8);
			if(has_seq) data.internal_timestamps[SHAKE_SENSOR_SK6_CAP1] = srps->seq;
			break;
		case SK6_RAW_DATA_ANALOG0:
			srps = (sk6_raw_packet_short*)rawpacket;
			data.ana0 = srps->data[0] + (srps->data[1] << 8);
			if(has_seq) data.internal_timestamps[SHAKE_SENSOR_ANA0] = srps->seq;
			break;
		case SK6_RAW_DATA_ANALOG1:
			srps = (sk6_raw_packet_short*)rawpacket;
			data.ana1 = srps->data[0] + (srps->data[1] << 8);
			if(has_seq) data.internal_timestamps[SHAKE_SENSOR_ANA1] = srps->seq;
			break;
		case SK6_RAW_DATA_EVENT:
			SHAKE_DBG("Parsing SK6_RAW_DATA_EVENT packet, %d\n", SK6_RAW_DATA_EVENT);
			srps = (sk6_raw_packet_short*)rawpacket;
			if(devpriv->navcb || devpriv->navcb_STDCALL) {
				int t = -1;
				SHAKE_DBG("Checking type of event before calling callback\n");
				switch(srps->data[0]) {
					case 1:
						t = SHAKE_NAV_NORMAL;
						break;
					case 2:
						t = SHAKE_NAV_UP;
						break;
					case 3:
						t = SHAKE_NAV_DOWN;
						break;
					case 4:
						t = SHAKE_NAV_CENTRE;
						break;
					case 5: 
						t = SK6_CS0_UPPER;
						break;
					case 6:
						t = SK6_CS0_LOWER;
						break;
					case 7: 
						t = SK6_CS1_UPPER;
						break;
					case 8:
						t = SK6_CS1_LOWER;
						break;
				}
				devpriv->lastevent = t;
				shake_thread_signal(&(devpriv->thread), CALLBACK_THREAD);
			}
			break;
		case SK6_RAW_DATA_SHAKING:
			SHAKE_DBG("Parsing SHAKE_RAW_DATA_SHAKING_EVENT\n");
			srpl = (sk6_raw_packet_long*)rawpacket;
			data.shaking_peak_accel = srpl->data[0] + (srpl->data[1] << 8);
			data.shaking_direction = srpl->data[2] + (srpl->data[3] << 8);
			data.shaking_timestamp = srpl->data[4] + (srpl->data[5] << 8);
			if(devpriv->navcb || devpriv->navcb_STDCALL) {
				devpriv->lastevent = SHAKE_SHAKING_EVENT;
				SHAKE_DBG("Calling callback for SHAKE_RAW_DATA_SHAKING\n");
				shake_thread_signal(&(devpriv->thread), CALLBACK_THREAD);
			}
			break;
		// this is the packet type for a microphone sample packet
		case SK6_RAW_DATA_AUDIO: {
			// ignore unless callback registered
			if(devpriv->audio_cb == NULL && devpriv->audio_cb_STDCALL == NULL)
				break;
			
			saud = (sk6_raw_packet_audio*)rawpacket;
			shake_mulaw_lookup(devpriv->audiobuf, saud->data, SHAKE_AUDIO_DATA_LEN);

			// call audio callback to handle microphone sample data. 
			if(devpriv->audio_cb)
				devpriv->audio_cb(dev, devpriv->audiobuf, SHAKE_AUDIO_DATA_LEN, NULL, 0);
			#ifdef _WIN32
			if(devpriv->audio_cb_STDCALL)
				devpriv->audio_cb_STDCALL(dev, devpriv->audiobuf, SHAKE_AUDIO_DATA_LEN, NULL, 0);
			#endif
			break;						   
		}
		// this is the packet type for a microphone sample packet from the extension module
		case SK6_RAW_DATA_AUDIO_EXP: {
			// ignore unless callback registered
			if(devpriv->audio_cb == NULL && devpriv->audio_cb_STDCALL == NULL)
				break;
			
			saud = (sk6_raw_packet_audio*)rawpacket;
			shake_mulaw_lookup(devpriv->audiobuf, saud->data, SHAKE_AUDIO_DATA_LEN);

			// call audio callback to handle microphone sample data. 
			if(devpriv->audio_cb)
				devpriv->audio_cb(dev, devpriv->audiobuf, SHAKE_AUDIO_DATA_LEN, NULL, 0);
			#ifdef _WIN32
			if(devpriv->audio_cb_STDCALL)
				devpriv->audio_cb_STDCALL(dev, devpriv->audiobuf, SHAKE_AUDIO_DATA_LEN, NULL, 0);
			#endif
			break;						   
		}
		// this is the packet type for an audio header, indicating that the next 
		// audio playback packet should now be sent
		case SK6_RAW_DATA_AUDIO_HEADER: {
			if(devpriv->audio_cb == NULL && devpriv->audio_cb_STDCALL == NULL) 
				break;

			// call audio callback to get next set of data from the application
			// (the data will be sent after this function returns), see shake_driver.c
			if(devpriv->audio_cb)
				devpriv->audio_cb(dev, NULL, 0, devpriv->playbackbuf, SHAKE_AUDIO_DATA_LEN);
			#ifdef _WIN32
			if(devpriv->audio_cb_STDCALL)
				devpriv->audio_cb_STDCALL(dev, NULL, 0, devpriv->playbackbuf, SHAKE_AUDIO_DATA_LEN);
			#endif
			break;
		}
	}

	return SHAKE_SUCCESS;
}

int SK6::classify_packet_header(char* packetbuf, int header_length, int ascii_packet) {
	int type = SHAKE_BAD_PACKET, i;

	SHAKE_DBG("classifying(): %c/%02X %c/%02X %c/%02X %c/%02X\n", packetbuf[0], packetbuf[0], packetbuf[1], packetbuf[1], packetbuf[2], packetbuf[2], packetbuf[3], packetbuf[3]);

	if(ascii_packet && (header_length != SK6_HEADER_LEN || packetbuf == NULL))
		return SHAKE_BAD_PACKET;
	if(!ascii_packet && (header_length != SK6_RAW_HEADER_LEN || packetbuf == NULL || packetbuf[0] != 0x7F || packetbuf[1] != 0x7F))
		return SHAKE_BAD_PACKET;

	if(ascii_packet) {
		i = 0;
	} else
		i = SK6_RAW_DATA_ACC;
	
	for(;i<SK6_NUM_PACKET_TYPES;i++) {
		if(ascii_packet) {
			if(strncmp(packetbuf, sk6_packet_headers[i], 4) == 0) {
				type = i;
				break;
			}
			if(i >= SK6_STARTUP_INFO)
				return SHAKE_BAD_PACKET;
		} else {
			if(packetbuf[2] == sk6_raw_packet_headers[i - SK6_RAW_DATA_ACC]) {
				type = i;
				break;
			}
		}
	}

	SHAKE_DBG("Packet classified as %d (%s)\n", type, sk6_packet_type_names[type]);
	return type;
}

int SK6::parse_ack_packet(char* packetbuf, int& addr, int& val) {
	if(!packetbuf) return SHAKE_ERROR;

	sk6_ack_packet* sak = (sk6_ack_packet*)packetbuf;

	addr = hex_ascii_to_int(sak->addr.data, 4, 4);
	val = hex_ascii_to_int(sak->val.data, 2, 2);

	return SHAKE_SUCCESS;
}

int SK6::read_device_info() {
	char buf[SK6_NUM_INFO_LINES][SK6_LINE_MAX];
	int len;
	int linecount;

	// read the initial byte (0x0A)
	//read_bytes(devpriv, buf[0], 1);

	// read each line
	for(linecount=0;linecount<SK6_NUM_INFO_LINES;linecount++) {
		memset(buf[linecount], 0, SK6_LINE_MAX);

		len = shake_read_info_line(devpriv, buf[linecount], SK6_LINE_MAX);
		if(len == -1)
			return SHAKE_ERROR;

		// zero remainder of buffer
		memset(buf[linecount] + len, 0, (SK6_LINE_MAX - len));

		if(linecount == SK6_FIRMWARE_REV) {
			int len = 0;
			// find start of revision number and get the value
			char* ptr = buf[linecount];
			while((*ptr != 0xA && *ptr != 0xD) && (*ptr < '0' || *ptr > '9')) {
				ptr++;
				len++;
			}
			sscanf(ptr, "%f", &(devpriv->fwrev));
			SHAKE_DBG("Got fwrev: %.2f\n", devpriv->fwrev);
		} else if(linecount == SK6_HARDWARE_REV) {

			// find start of revision number and get the value
			char* ptr = buf[linecount];
			while(*ptr < '0' || *ptr > '9')
				ptr++;
			sscanf(ptr, "%f", &(devpriv->hwrev));
			SHAKE_DBG("Got hwrev: %.2f\n", devpriv->hwrev);
		} else if(linecount == SK6_SERIAL_NUMBER) {
			int spacecount = 0, i;

			// find start of serial number and read the remainder of the string
			char* ptr = buf[linecount];
			while(spacecount < 2) {
				if(*ptr == ' ')
					spacecount++;
				ptr++;
			}
			i = 0;
			memset(devpriv->serial, 0, 20);
			for(;*ptr != '\0';ptr++) {
				devpriv->serial[i] = *ptr;
				i++;
			}
			SHAKE_DBG("Got serial: %s\n", devpriv->serial);
		} else if (linecount == SK6_EXPANSION_SLOT1) {
			int i;
			char* module = buf[linecount];
			for(i=SK6_MODULE_NONE;i<=SK6_MODULE_EXPANSION;i++) {
				if(strncmp(module, SK6_modules[i], strlen(SK6_modules[i])) == 0) {
					devpriv->modules[0] = i;
					break;
				}
			}
			SHAKE_DBG("Got slot1: %d\n", devpriv->modules[0]);
		} else if (linecount == SK6_EXPANSION_SLOT2) {
			int i;
			char* module = buf[linecount];
			for(i=SK6_MODULE_NONE;i<=SK6_MODULE_EXPANSION;i++) {
				if(strncmp(module, SK6_modules[i], strlen(SK6_modules[i])) == 0) {
					devpriv->modules[1] = i;
					break;
				}
			}
			SHAKE_DBG("Got slot2: %d\n", devpriv->modules[1]);
		}

	}

	// read trailing \n
	char c;
	read_bytes(devpriv, &c, 1);

	return SHAKE_SUCCESS;
}
