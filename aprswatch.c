// Process AX.25 frames containing APRS data, feed to APRS2 network
// Copyright 2018-2023, Phil Karn, KA9Q

#define _GNU_SOURCE 1
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>

#include <locale.h>
#include <errno.h>
#include <sys/socket.h>
#include <netdb.h>
#include <math.h>
#include <ctype.h>
#if __linux__
#include <bsd/string.h>
#else
#include <string.h>
#endif
#include <sysexits.h>

#include "multicast.h"
#include "ax25.h"
#include "misc.h"

char *Mcast_address_text = "ax25.local";
char *Host = "noam.aprs2.net";
char *Port = "14580";
char *User;
char *Passcode;
char *Logfilename;
FILE *Logfile;
const char *App_path;
int Verbose;

int Input_fd = -1;
int Network_fd = -1;

//pthread_t Read_thread;
//void *netreader(void *arg);

int main(int argc,char *argv[]){
  App_path = argv[0];
  // Quickly drop root if we have it
  // The sooner we do this, the fewer options there are for abuse
  if(seteuid(getuid()) != 0)
    fprintf(stderr,"seteuid: %s\n",strerror(errno));
  
  setlocale(LC_ALL,getenv("LANG"));
  setlinebuf(stdout);

  int c;
  //while((c = getopt(argc,argv,"u:p:I:vh:f:V")) != EOF){
  while((c = getopt(argc,argv,"I:vh:V")) != EOF){
    switch(c){
    /*case 'f':
      Logfilename = optarg;
      Verbose = 0;
      break;
    case 'u':
      User = optarg;
      break;
      */
    case 'v':
      if(!Logfilename)
	Verbose++;
      break;
    case 'h':
      Host = optarg;
      break;
/*    case 'p':
      Passcode = optarg;
      break;
      */
    case 'I':
      Mcast_address_text = optarg;
      break;
    case 'V':
      VERSION();
      exit(EX_OK);
    default:
      //fprintf(stderr,"Usage: %s -u user [-p passcode] [-v] [-I mcast_address][-h host]\n",argv[0]);
      fprintf(stderr,"Usage: %s [-v] [-I mcast_address][-h host]\n",argv[0]);
      exit(EX_USAGE);
    }
  }
  // Set up multicast input
  {
    struct sockaddr_storage sock;
    printf("Resolving multicast address: %s\n", Mcast_address_text);
    resolve_mcast(Mcast_address_text, &sock, DEFAULT_RTP_PORT, NULL, 0, 0);
    printf("Creating network socket\n");
    Input_fd = listen_mcast(&sock,NULL);
  }
  if(Input_fd == -1){
    fprintf(stderr,"Can't set up multicast input from %s\n",Mcast_address_text);
    exit(EX_IOERR);
  }

  Logfile = stdout;

  if(Logfile){
    setlinebuf(Logfile);
    fprintf(Logfile,"APRS watcher program by KA9Q\n");
  }

  while(1) {
    printf("Listening to multicast address: %s\n", Mcast_address_text);
    uint8_t packet[2048];
    int size;
    while((size = recv(Input_fd,packet,sizeof(packet),0)) > 0){
      struct rtp_header rtp_header;
      uint8_t const *dp = packet;
      
      dp = ntoh_rtp(&rtp_header,dp);
      size -= dp - packet;
      
      if(rtp_header.pad){
	// Remove padding
	size -= dp[size-1];
	rtp_header.pad = 0;
      }

      if(size <= 0)
	continue;  // Bogus RTP header?
      
      if(rtp_header.type != AX25_pt)
	continue; // Wrong type
      
      
      // Parse incoming AX.25 frame
      struct ax25_frame frame;
      if(ax25_parse(&frame,dp,size) < 0){
	if(Logfile)
	  fprintf(Logfile," Unparsable packet\n");
	continue;
      }
      
      // Construct TNC2-style monitor string for APRS reporting
      char monstring[2048]; // Should be large enough for any legal AX.25 frame; we'll assert this periodically
      int sspace = sizeof(monstring);
      int infolen = 0;
      //int is_tcpip = 0;
      {
	memset(monstring,0,sizeof(monstring));
	char *cp = monstring;
	{
	  int w = snprintf(cp,sspace,"%s>%s",frame.source,frame.dest);
	  cp += w; sspace -= w;
	  assert(sspace > 0);
	}
	for(int i=0;i<frame.ndigi;i++){
	  // if "TCPIP" appears, this frame came off the Internet and should not be sent back to it
	  //if(strcmp(frame.digipeaters[i].name,"TCPIP") == 0)
	  //  is_tcpip = 1;
	  int const w = snprintf(cp,sspace,",%s%s",frame.digipeaters[i].name,frame.digipeaters[i].h ? "*" : "");
	  cp += w; sspace -= w;
	  assert(sspace > 0);
	}
	{
	  // qAR means a bidirectional i-gate, qAO means receive-only
	  //    w = snprintf(cp,sspace,",qAR,%s",User);
	  //int const w = snprintf(cp,sspace,",qAO,%s",User);
	  int const w = 0;

	  cp += w; sspace -= w;
	  *cp++ = ':'; sspace--;
	  assert(sspace > 0);
	}      
	for(int i=0; i < frame.info_len; i++){
	  char const c = frame.information[i] & 0x7f; // Strip parity in monitor strings
	  if(c != '\r' && c != '\n' && c != '\0'){
	    // Strip newlines, returns and nulls (we'll add a cr-lf later)
	    *cp++ = c;
	    sspace--;
	    infolen++;
	    assert(sspace > 0);
	  }
	}
	*cp++ = '\0';
	sspace--;
      }      
      assert(sizeof(monstring) - sspace - 1 == strlen(monstring));

      char result[1024];
      printf(" %s ssrc %u seq %d %s\n", format_gpstime(result,sizeof(result),gps_time_ns()), rtp_header.ssrc, rtp_header.seq, monstring);
      
    }
  }
}
