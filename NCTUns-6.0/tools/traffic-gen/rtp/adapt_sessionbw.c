/*
 * Copyright (c) from 2000 to 2009
 * 
 * Network and System Laboratory 
 * Department of Computer Science 
 * College of Computer Science
 * National Chiao Tung University, Taiwan
 * All Rights Reserved.
 * 
 * This source code file is part of the NCTUns 6.0 network simulator.
 *
 * Permission to use, copy, modify, and distribute this software and
 * its documentation is hereby granted (excluding for commercial or
 * for-profit use), provided that both the copyright notice and this
 * permission notice appear in all copies of the software, derivative
 * works, or modified versions, and any portions thereof, and that
 * both notices appear in supporting documentation, and that credit
 * is given to National Chiao Tung University, Taiwan in all publications 
 * reporting on direct or indirect use of this code or its derivatives.
 *
 * National Chiao Tung University, Taiwan makes no representations 
 * about the suitability of this software for any purpose. It is provided 
 * "AS IS" without express or implied warranty.
 *
 * A Web site containing the latest NCTUns 6.0 network simulator software 
 * and its documentations is set up at http://NSL.csie.nctu.edu.tw/nctuns.html.
 *
 * Project Chief-Technology-Officer
 * 
 * Prof. Shie-Yuan Wang <shieyuan@csie.nctu.edu.tw>
 * National Chiao Tung University, Taiwan
 *
 * 09/01/2009
 */

/*
 * codec_name - the name should be limit in 16 chars.
 * start_time, stop_time for this application is setup by NCTUns.
 *
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/param.h>
#include <netdb.h>
#include <unistd.h>


#include <rtp.h>
#include <rtp_session_api.h>

	/*
	 *	below is the mechanism for dynamic adjustment of the bandwidth, which reference by
	 *	Busse, I., Dener, B. and H. Schulzrinne, "Dynamic QoS Control of Multimedia Applications
	 *	Based on RTP", Computer Communications, vol. 19, pp. 49{58, January 1996.
	 *
	 *	The program change the protocol from TCP to UDP and simulate the traffic.
	 *
	 * parameter:
	 *
	 *	the number of receivers in the unloaded state "Nu", 
	 *	the number of receivers in the loaded state "Nl", 
	 *	and the number of receivers in the congested state "Nc" 
	 *	the total number of receivers "N".
	 *
	 *	"Br" :	the actual bandwidth as reported in the most recent RTCP sender report.
	 *	"Ba" :	the allowed bandwidth that can be used by the multimedia application.
	 *	"Ba", "Br"  :	belong [max_bw, min_bw]
	 *
	 * algo:
	 *	if [ ( Nc / N ) >= Nd ], then descision <- DECREASE.
	 *	else if [ ( Nl / N ) >= Nh ], descision <- then HOLD.
	 *	else, descision <- INCREASE
	 *
	 *	if descision = DECREASE, then "Ba" <- max{"Br" * "D", min_bw}
	 *	else if descision = INCREASE, then "Ba" <- min{"Br" + "I", max_bw}
	 */
	
	/*
	 * the enverionment we set
	 * 
	 * codec name :	nv, sampling rate = 90,000 Hz, 3000 samples/frame,
	 *		33.33... ms/frame, 0.55... bits/sample, 1666.66... bits/frame (208.33... bytes/frame)
	 */
	
	double	min_bw = 50, max_bw = 1000, cur_bw;	/* UNIT - k-bits/sec */
	double	D = 0.875, I = 50;		/* "I" = 50 Kbis =  */
	double	upper = 0.1, lower = 0.05;	/* upper threshold(congested) = 10 %, lower threshold(loaded) = 5% */
	int	minbw_pktsize, packet_size;
	double	Nd = 0.1, Nh = 0.1, Nc, Nl, N;
	u_int32	src[10];
	double	pktlossrate[10];


char			*local_ip, *remote_ip, *cname;
rtperror		err;			/* the error that RTP lib returns */
protocol_t		proto = udp;		/* the protocol that rtp(rtcp) use */	
int			local_port, remote_port, i = 0;
u_int8			ttl = 0;		/* it is only useful in mlticast that will be omit in unicast case */
double			session_bw;
char			sendbuf[RTP_MAX_PKT_SIZE], recvbuf[RTP_MAX_PKT_SIZE], *codec_name, marker = 1, *reason = "byebye!";
double			rtp_start_time, rtp_stop_time, rtp_interval, starttime, nexttime, now;
int			codec_num, i, packet_size, is_audio;
int			addtimestamp, reads, nfds = 0, recbuflen = RTP_MAX_PKT_SIZE;
double			bits_per_sample, delay, ms_pkt, framerate, sampling_rate;
struct	timeval		start_tv, now_tv, timeout_tv, nexttime_tv;
fd_set			afds, rfds;
socktype		rtp_sockt, rtcp_sockt;	/* track of socket, and assign to RTP, RTCP sockets */
rtperror		err;			/* the error that RTP lib returns */
session_id		sid;			/* we can track the rtp by session (id) */
u_int32			ssrc;

void			err_handle(rtperror err);


void
sdp_parse(session_id sid, char **argv){

	FILE	*fp;
	char	line[50], *token;

	fp = fopen(argv[4], "r");
	if (fp) {
		while (fgets(line, 50, fp) != NULL) {	// Reading stops after an EOF or a newline. A "\0" is stored after the last character in the buffer.
			token = strtok(line, "\t\n ");
			if (!strncmp(token, "e=", 2)) {	// email address
				char	*tmp;
				tmp = token + 2;
				token = strtok(NULL, "\t\n ");
				err = rtp_set_sdes_item(sid, RTCP_SDES_EMAIL, tmp, atoi(token));
        			if (err) {
                			err_handle(err);
       	 			}
				printf("set email=%s with interval %d\n", tmp, atoi(token));
			}
			else if (!strncmp(token, "p=", 2)) {	// phone number
				char	*tmp;
				tmp = token + 2;
				token = strtok(NULL, "\t\n ");
				err = rtp_set_sdes_item(sid, RTCP_SDES_PHONE, tmp, atoi(token));
        			if (err) {
                			err_handle(err);
       	 			}
				printf("set phone=%s with interval %d\n", tmp, atoi(token));
			}
			else if (!strncmp(token, "b=AS:", 5)) {
				char	*tmp;
				tmp = token + 5;
				session_bw = atof(tmp);
				err = rtp_set_session_bw(sid, session_bw);	/* set session bandwidth for this application */
        			if (err) {
                			err_handle(err);
       	 			}
				printf("set session bandwidth=%lf\n", session_bw);
			}
			else if (!strncmp(token, "t=", 2)) {	// time the session is active, NOTE! max t = 4200 in NCTUns.
				char	*tmp;
				tmp = token + 2;
				rtp_start_time = atof(tmp);
				token = strtok(NULL, "\t\n ");
				rtp_stop_time = atof(token);
				printf("rtp_start_time = %lf, rtp_stop_time = %lf\n", rtp_start_time, rtp_stop_time);
			}
			else if (!strncmp(token, "r=", 2)) {	// repeat interval, active duration, list of offset from start-time
				char	*tmp;
				tmp = token + 2;
			}
			else if (!strcmp(token, "m=audio")) {	// media
				is_audio = 1;
				token = strtok(NULL, "\t\n ");
				remote_port = atoi(token);
				token = strtok(NULL, "\t\n ");
				token = strtok(NULL, "\t\n ");
				codec_num = atoi(token);
			}
			else if (!strcmp(token, "m=video")) {	// media
				is_audio = 0;
				token = strtok(NULL, "\t\n ");
				remote_port = atoi(token);
				token = strtok(NULL, "\t\n ");
				token = strtok(NULL, "\t\n ");
				codec_num = atoi(token);
			}
			else if (!strncmp(token, "a=rtpmap:", 9)) {	// email address
				char	*tmp, *temp, *rate;
				tmp = token + 9;
				codec_num = atoi(tmp);
				token = strtok(NULL, "\t\n ");
				tmp = strchr(token, '/');
				codec_name = strndup(token, (tmp - token));
				temp = strchr((tmp + 1), '/');
				rate = strndup((tmp + 1), (temp - (tmp + 1)));
				sampling_rate = atof(rate);
				free(rate);
				bits_per_sample = atof(temp + 1);
				printf("codec_num = %d, codec_name = %s, sampling_rate = %lf, bits_per_sample = %lf\n", codec_num, codec_name, sampling_rate, bits_per_sample);
			}
			else if (!strncmp(token, "a=ptime:", 8)) {	// email address
				char	*tmp;
				tmp = token + 8;
				ms_pkt = atof(tmp);
				printf("ms_pkt = %lf\n", ms_pkt);
			}
			else if (!strncmp(token, "a=framerate:", 12)) {	// email address
				char	*tmp;
				tmp = token + 12;
				framerate = atof(tmp);
				printf("frames/sec = %lf\n", framerate);
			}
			else if (!strcmp(token, "c=IN")) {	// email address
				char	*tmp;
				tmp = token + 4;
				token = strtok(NULL, "\t\n ");
				if (!strcmp(token, "IP4")) {	// email address
					token = strtok(NULL, "\t\n ");
					if (strncmp(token, local_ip, 15) != 0) {
						err = rtp_add_dest_list(sid, token, remote_port, ttl, proto, proto);	// set send addr
						if (err) {
							err_handle(err);
						}
						printf("add send address %s\n", token);
					}
				}
			}
			else {	// unknown type, ignore it.
			}
		}
	}
	else {
		fprintf(stderr, "Can't open file - %s , process terminated\n", argv[4]);
		exit(1);
	}
}

	
void 
err_handle(rtperror err){
	if (err < 0) {/* warning */
		fprintf(stderr, "%s\n", RTPStrError(err));
	}
	else if (err) {/* error */
		fprintf(stderr, "%s\n", RTPStrError(err));	
		exit(1);
	}		
}


double 
get_cur_bw(int pkt_size){
	return ( packet_size * 8. * 30. / 1000. ); /* pkt_size * 8 (byte->bit) * (framerate) / 1000 (bit->k-bits)  */
}


void 
match_ssrc(u_int32 s, double rate){
	int	j;

	for (j = 0; j < 10; j++) {
		if (src[j] != 0){
			if (src[j] == s) {
				pktlossrate[j] = rate;
			}
		}
		else{
			src[j] = s;
			pktlossrate[j] = rate;
			N++;
			break;
		}
	}
}


int
adapt(){
	int	j;

	Nc = 0;
	Nl = 0;

	for (j = 0; j < 10; j++) {
		if (src[j] != 0){
			if ( (upper > pktlossrate[j]) && (pktlossrate[j] > lower) ) {
				fprintf(stderr, "pktlossrate[%d]=%lf, so Nl++\n", j, pktlossrate[j]);
				Nl++;
			}
			else if (pktlossrate[j] > upper) {
				Nc++;
			}
		}
		else {
			break;
		}
	}

	if ( (Nc / N) >= Nd ) {
		fprintf(stderr, "Nc : %lf, N : %lf, Nd : %lf, (Nc / N) : %lf\n", Nc, N, Nd, (Nc / N));
		return 1;
	}
	else if ( (Nl / N) >= Nh ) {
		fprintf(stderr, "Nl : %lf, N : %lf, Nh : %lf, (Nl / N) : %lf\n", Nl, N, Nh, (Nl / N));
		return 0;
	}
	else {
		return 2;
	}
}


void 
adapt_bw(double lossrate){

	int	j;
	match_ssrc(ssrc, lossrate);
	j = adapt();

	if (j == 1) {
		if ((cur_bw * D) > min_bw) {
			packet_size = packet_size * D;
			cur_bw = get_cur_bw(packet_size);
			fprintf(stderr, "case1 - pkt lossrate : %lf > upper : %lf, decrease cur_bw to = %lf\n", lossrate, upper, cur_bw);
			printf("case2 - pkt lossrate : %lf > upper : %lf, decrease cur_bw to = %lf\n", lossrate, upper, cur_bw);
		}
		else {
			packet_size = minbw_pktsize;
			cur_bw = get_cur_bw(packet_size);
			fprintf(stderr, "case2 - pkt lossrate : %lf > upper : %lf, decrease cur_bw to = %lf\n", lossrate, upper, cur_bw);
			printf("case2 - pkt lossrate : %lf > upper : %lf, decrease cur_bw to = %lf\n", lossrate, upper, cur_bw);
		}
	}
	else if (j == 2) {
		if (cur_bw + I < max_bw) {
			packet_size  = packet_size + minbw_pktsize;
			cur_bw = get_cur_bw(packet_size);
			fprintf(stderr, "case3 - pkt lossrate : %lf < lower : %lf, increase cur_bw to = %lf\n", lossrate, lower, cur_bw);
			printf("case3 - pkt lossrate : %lf < lower : %lf, increase cur_bw to = %lf\n", lossrate, lower, cur_bw);
		}
		else {
			packet_size = minbw_pktsize * 20;
			cur_bw = get_cur_bw(packet_size);
			fprintf(stderr, "case4 - pkt lossrate : %lf < lower : %lf, increase cur_bw to = %lf\n", lossrate, lower, cur_bw);
			printf("case4 - pkt lossrate : %lf < lower : %lf, increase cur_bw to = %lf\n", lossrate, lower, cur_bw);
		}
	}
	else {
		fprintf(stderr, "case5 - pkt lossrate : %lf,  cur_bw is %lf\n", lossrate, cur_bw);
		printf("case5 - pkt lossrate : %lf, cur_bw is %lf\n", lossrate, cur_bw);
	}
}


int
initial_session(int argc, char **argv, session_id sid){

	for (i = 0; i < 10; i ++) {
		src[i] = 0;
	}

        err = rtp_add_sour_list(sid, local_ip, local_port, proto, proto);       /* set receive addr */
        if (err) {
                err_handle(err);
        }

        err = rtp_set_sdes_item(sid, RTCP_SDES_CNAME, cname, 1);        /* for CNAME SDES */
        if (err) {
                err_handle(err);
        }

        /* add the RTP Session's host address to send to and setup the socket. (NOTE : port != 0)*/
        err = rtp_open_connection(sid);
        if (err) {
                err_handle(err);
        }

        return 0;
}


int main(int argc, char **argv){

        /* setting commad line information */
        local_ip        = argv[1];
        local_port      = atoi(argv[2]);
        cname           = argv[3];
        proto           = udp;

	/* create a rtp session, we will assign unique value to identify */
	err = rtp_create(&sid);	
	if (err) {
		err_handle(err);
	}

	sdp_parse(sid, argv);

	if (rtp_start_time > 0) {
		usleep( ((int) rtp_start_time * 1000000) );
	}

        if ((i = initial_session(argc, argv, sid))) {
                printf("WARNING : initial_session warning = %d\n", i);
        }
        else {
                printf("initial_session is ok\n");
        }

	if (is_audio) {
		delay	= (ms_pkt / 1000.);
	}
	else {
		delay	= (1. / framerate);	// unit: ms/sec. we assume that 300 samples/frame, 
	}

	/* bytes of each packet = ((bits/sample) / 8 ) * (clock rate) * ( each delay of packet in sec ) */
	packet_size = (int) ( bits_per_sample * sampling_rate * delay / 8.);
	
	/* original_bw = min_bw = 50, so we use the same pkt size */
	minbw_pktsize = packet_size;
	
	printf("bits_per_sample = %lf, (bits_per_sample/8.) = %lf\n", bits_per_sample, (bits_per_sample/8.));
	printf("sampling_rate = %lf\n", sampling_rate);
	printf("delay = %lf\n", delay);
	printf("packet_size = %d\n", packet_size);
	fflush(stdout);
	
	/* the bandwidth wa are using, 30 = frames/sec */
	cur_bw = get_cur_bw(packet_size);
	
	err = rtp_get_sour_rtpsocket(sid, &rtp_sockt);
	if (err) {
		err_handle(err);
	}
	err = rtp_get_sour_rtcpsocket(sid, &rtcp_sockt);
	if (err) {
		err_handle(err);
	}	
	
	if (rtp_sockt > nfds || rtcp_sockt > nfds) {
		if (rtp_sockt > rtcp_sockt)
			nfds = rtp_sockt;
		else
			nfds = rtcp_sockt;
	}
	
	FD_ZERO(&afds);
	FD_ZERO(&rfds);
	FD_SET(rtp_sockt, &afds);
	FD_SET(rtcp_sockt, &afds);
	
	gettimeofday(&start_tv, NULL);
	starttime = (start_tv.tv_sec + start_tv.tv_usec / 1000000.);
	nexttime = starttime;
	
	rtp_interval = rtp_stop_time - rtp_start_time;
	
	printf("rtp_interval = %f\n", rtp_interval);
	fflush(stdout);
	
	while (rtp_interval >= 0) {
		
		memcpy(&rfds, &afds, sizeof(rfds));
		
		if (RTP_MAX_PKT_SIZE < packet_size){
			fprintf(stderr, "RTP_MAX_PKT_SIZE < reads\n");
			continue;
		}
		
		addtimestamp = ((int) packet_size * (bits_per_sample / 8.) );

		err = rtp_send(sid, marker, addtimestamp, codec_num, (int8 *)sendbuf, packet_size);
		if (err) {
			err_handle(err);
		}
		
		marker = 0;	/* not the first packet of talkspurt */
		rtp_interval -= delay;
		nexttime += delay;
		gettimeofday(&now_tv, NULL);
		now = (now_tv.tv_sec + now_tv.tv_usec / 1000000.);
		
		err = get_rtcp_timeout(sid, &timeout_tv);
		if (err) {
			err_handle(err);
		}
		
		while (now < nexttime) {	/* send next packet until now >= nexttime */
		
			//printf("now = %lf\n", now);
			//printf("nexttime = %lf\n", nexttime);
			
			if (time_expire(&timeout_tv, &now_tv)) { 
				
				err = rtp_check_on_expire();
				if (err) {
					err_handle(err);
				}
				
				err = get_rtcp_timeout(sid, &timeout_tv);
				if (err) {
					err_handle(err);
				}
				printf("timeval_to_double(timeout_tv) = %lf\n", timeval_to_double(timeout_tv));	
			}
			
			/* BECAREFUL, if we disable RTCP, the timeval we get will be 0 */
			if (timeval_to_double(timeout_tv) == 0 || nexttime < timeval_to_double(timeout_tv)) {
				nexttime_tv = double_to_timeval(nexttime - now);
			}
			else {
				nexttime_tv = double_to_timeval(timeval_to_double(timeout_tv) - now);
			}
			
			if (select(nfds + 1, &rfds, (fd_set *)0, (fd_set *)0, &nexttime_tv) < 0) {
				if (errno == EINTR)
					continue;
				else {	
					printf("nexttime_tv.tv_sec = %ld\n", nexttime_tv.tv_sec);
					printf("nexttime_tv.tv_usec = %ld\n", nexttime_tv.tv_usec);
					printf("select error: %d\n", errno);
					//exit(1);
				}
			}
			
			if (FD_ISSET(rtp_sockt, &rfds)) {

				err = on_receive(sid, rtp_sockt, recvbuf, &recbuflen);
				if (err) {
					err_handle(err);
				}	
			}
			else if (FD_ISSET(rtcp_sockt, &rfds)) {
			
				err = on_receive(sid, rtcp_sockt, recvbuf, &recbuflen);
				if (err) {
					err_handle(err);
				}
				
				adapt_bw(get_recent_recv_loosrate(sid, &ssrc));
			}
			
			gettimeofday(&now_tv, NULL);
			now = (now_tv.tv_sec + now_tv.tv_usec / 1000000.);
		} // while(now < nexttime)
	} // while (interval)
	
	err = rtp_close_connection(sid, reason);
	if (err) {
		err_handle(err);
	}
	
	err = rtp_delete(sid);
	if (err) {
		err_handle(err);
	}
			
	return 0;
}
