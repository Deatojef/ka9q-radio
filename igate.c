// igate
//
// Functionally just an Rx-only igate. 
// Process AX.25 frames containing APRS data, feed to APRS2 network
//
// Copyright 2017-2024 Phil Karn, KA9Q & Jeff Deaton, N6BA
// Major revisions fall 2020, 2023, 2024 (really continuous revisions!)

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
#include <signal.h>
#include <sysexits.h>
#include <poll.h>
#include <gps.h>

#include "multicast.h"
#include "ax25.h"
#include "misc.h"


// structures for APRS telemetry data and station particulars
// ---------------------------------------------------------------------
typedef struct {
    int a;
    int b;
    int c;
} APRS_COEF;

typedef struct {
    APRS_COEF rx;
    APRS_COEF drp;
    APRS_COEF rxsat;
} APRS_EQNS;

typedef struct {
    double lat;
    double lon;
    double alt;
    double course;
    double speed;
    char symbol[3];  // character representing the symbol
    char overlay[3]; // character representing the char to be overlaid on top of the symbol
    char comment[186]; // max APRS information field length is 256 bytes. That minus normal position packet length (ex. 70 bytes), leaves 186 bytes for "everything else" like the station comment.
} APRS_STATION;

// ---------------------------------------------------------------------


// global variables
// ---------------------------------------------------------------------
char *Mcast_address_text = "ax25.local";
char *Host = "noam.aprs2.net";
char *Port = "14580";
char *User;
char *Passcode;
char *Logfilename;
char *Latitude;
char *Longitude;
char *Altitude;
char *Comment;
char *Mobile = "0";
char *Beaconing = "0";
char *TelemSeqFilename;
char *GPSDHost = NULL;

// max packet size for position packets (actually the max size is 256 - 70 = 186, but backing this off by 8 bytes for a little buffer room)
static int MAX_POSIT = 178;

// By default, when beaconing to APRS-IS, we represent this station as an Rx-only Igate.
char *Symbol = "&";
char *Overlay = "R";

// for keeping track of the number of received and dropped (because of bad stuff) packets
int dropped_pkts = 0;
int received_pkts = 0;
int received_sat_pkts = 0;

// Starting APRS telemetry sequence number.  According to APRS this should increment by 1 for each batch of telemetry data sent.
int sequence = 0;

// APRS tocall value.  Experimental tocalls start with APZxxx.  Using "KR1" for "KA9Q-Radio 1".  Dunno...just making this up.  ;)
char *tocall = "APZKR1";

// By default beaconing to APRS-IS is not enabled.
bool beaconing_enabled = false;

// By default, not using GPSD for location information
bool gpsd_enabled = false;
int gpsmode;

// station information
APRS_STATION aprs_station;

// logfile and application name
FILE *Logfile;
const char *App_path;

// Telemetry sequence file handle
FILE *telefile;

// Verbosity level
int Verbose;

// File descriptors
int Input_fd = -1;
int Network_fd = -1;

// The global variable that processing loops check.  If this is set to "true", then processing loops should end.
bool stop_processing = false;

// is this a statis or mobile station.  Static by default
bool ismobile = false;
// ---------------------------------------------------------------------


// Thread mutex, conditions, and threads
// ---------------------------------------------------------------------
pthread_mutex_t tcp_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t beacon_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t aprs_station_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t cleartoxmit = PTHREAD_COND_INITIALIZER;
pthread_mutex_t stats_lock = PTHREAD_MUTEX_INITIALIZER;

pthread_t Beacon_thread;
pthread_t Read_thread;
pthread_t GPSD_thread;
// ---------------------------------------------------------------------


// Functions
// ---------------------------------------------------------------------
void *netreader(void *arg);
void *beaconthread(void *arg);
void *gpsthread(void *arg);
int positpacket(char *buffer, size_t n, char *info);
int bitspacket(char *buffer, size_t n);
int unitspacket(char *buffer, size_t n);
int parampacket(char *buffer, size_t n);
int eqnpacket(char *buffer, size_t n, APRS_EQNS *eqns);
int calculatecoef(int value, APRS_COEF *c);
int telemetrypacket(char *buffer, size_t n, APRS_EQNS *eqns);
int createinfostring(char *buffer, size_t n, APRS_STATION *station);
void initstation(APRS_STATION *station);
void closedown(int x);
int readtelemseq(char *filename);
void writetelemseq(char *filename, int seq);
// --------------------------------------------------------------------


// ---------------------------------------------------------------------
// ---------------------------------------------------------------------
// main function
// ---------------------------------------------------------------------
// ---------------------------------------------------------------------
int main(int argc,char *argv[])
{
    char timebuffer[1024];

    App_path = argv[0];
    // Quickly drop root if we have it
    // The sooner we do this, the fewer options there are for abuse
    if(seteuid(getuid()) != 0)
        fprintf(stderr,"seteuid: %s\n",strerror(errno));

    memset(timebuffer, 0, sizeof(timebuffer));
    setlocale(LC_ALL,getenv("LANG"));
    setlinebuf(stdout);

    // capture any sort of kill/close conditions.  Mostly so we can cleanly exit
    signal(SIGPIPE, closedown);
    signal(SIGINT, closedown);
    signal(SIGKILL, closedown);
    signal(SIGQUIT, closedown);
    signal(SIGTERM, closedown);

    // Parse through command line arguments
    int c;
    while((c = getopt(argc,argv,"u:p:I:vh:f:L:G:A:S:O:C:V:B:T:H:M:")) != EOF) {
        switch(c) {
            case 'f':
                Logfilename = optarg;
                Verbose = 0;
                break;
            case 'u':
                User = optarg;
                break;
            case 'v':
                if(!Logfilename)
                    Verbose++;
                break;
            case 'h':
                Host = optarg;
                break;
            case 'p':
                Passcode = optarg;
                break;
            case 'I':
                Mcast_address_text = optarg;
                break;
            case 'L':
                Latitude = optarg;
                break;
            case 'G':
                Longitude = optarg;
                break;
            case 'A':
                Altitude = optarg;
                break;
            case 'C':
                Comment = optarg;
                break;
            case 'S':
                Symbol = optarg;
                break;
            case 'O':
                Overlay = optarg;
                break;
            case 'B':
                Beaconing = optarg;
                break;
            case 'T':
                TelemSeqFilename = optarg;
                break;
            case 'H':
                GPSDHost = optarg;
                break;
            case 'M':
                Mobile = optarg;
                break;
            case 'V':
                VERSION();
                exit(EX_OK);
            default:
                fprintf(stderr,"Usage: %s -u user [-p passcode] [-v] [-I mcast_address][-h host] [-L latitude] [-G longitude] [-A altitude] [-C comment string] [-S symbol char] [-O overlay char] [-B 0|1] [-T telemetry sequence filename] [-H gpsd hostname] [-M 0|1]\n",argv[0]);
                exit(EX_USAGE);
        }
    }


    if(Logfilename)
        Logfile = fopen(Logfilename,"a");
    else if (Verbose)
        Logfile = stdout;

    if (Logfile) {
        setlinebuf(Logfile);
        format_gpstime(timebuffer, sizeof(timebuffer), gps_time_ns());
        fprintf(Logfile,"%s ################## START:  igate ##############\n", timebuffer);
    }
    else {
        fprintf(stderr, "Unable to write to log file:  %s\n", Logfilename);
        exit(EX_IOERR);
    }

    if(User == NULL) {
        fprintf(stderr,"Must specify a callsign for to use as the user when igating to APRS-IS:  -u User\n");
        exit(EX_USAGE);
    }


    if(!Passcode) {
        // Calculate trivial hash authenticator
        int hash = 0x73e2;
        char callsign[11];
        strlcpy(callsign,User,sizeof(callsign));
        char *cp;
        if((cp = strchr(callsign,'-')) != NULL)
            *cp = '\0';

        int const len = strlen(callsign);

        for(int i=0; i<len; i += 2) {
            hash ^= toupper(callsign[i]) << 8;
            hash ^= toupper(callsign[i+1]);
        }
        hash &= 0x7fff;
        if(asprintf(&Passcode,"%d",hash) < 0) {
            fprintf(stderr,"Unexpected error in computing passcode\n");
            exit(EX_SOFTWARE);
        }
    }

    // initialize our station details
    initstation(&aprs_station);

    // mobile station or static?
    format_gpstime(timebuffer, sizeof(timebuffer), gps_time_ns());
    int m = atoi(Mobile);
    if (m) {
        ismobile = true;
        fprintf(Logfile, "%s Mobile station behavior selected.\n", timebuffer);
    }
    else 
        fprintf(Logfile, "%s Static station behavior selected.\n", timebuffer);


    // Check if beaconing was enabled
    int b = atoi(Beaconing);

    if (b > 0) { // beaconing was enabled

        beaconing_enabled = true;

        // get the starting sequence number for telemetry packets sent to the APRS-IS server
        sequence = readtelemseq(TelemSeqFilename);

        // get timestamp for printing output below
        format_gpstime(timebuffer, sizeof(timebuffer), gps_time_ns());
        fprintf(Logfile, "%s Beaconing to %s enabled.  Initial telemetry sequence: %d\n", timebuffer, Host, sequence);

        // check the symbol/overlay/comment fields
        if (Symbol && Overlay && Comment) {
            strncpy(aprs_station.symbol, Symbol, sizeof(aprs_station.symbol)-1);
            strncpy(aprs_station.overlay, Overlay, sizeof(aprs_station.overlay)-1);
            strncpy(aprs_station.comment, Comment, sizeof(aprs_station.comment)-1);
            fprintf(Logfile, "%s Station identification:  symbol=%s, overlay=%s, comment=%s\n", timebuffer, Symbol, Overlay, Comment);
        }

        if (GPSDHost == NULL && Latitude && Longitude && Altitude) { // we're using the provided LAT, LON, ALT parameters

            gpsd_enabled = false;

            // populate this station's location details
            aprs_station.lat = atof(Latitude);
            aprs_station.lon = atof(Longitude);
            aprs_station.alt = atof(Altitude);

            fprintf(Logfile, "%s Using provided latitude=%s, longitude=%s, altitude=%s\n", timebuffer, Latitude, Longitude, Altitude);
        }
        else if (GPSDHost) {
            gpsd_enabled = true;
            fprintf(Logfile, "%s Using GPSD on %s for location of this station.\n", timebuffer, GPSDHost);
        }
    }
    else {  // beaconing was not enabled
        beaconing_enabled = false;
        fprintf(Logfile, "%s Not configured to beacon to APRS-IS.\n", format_gpstime(timebuffer, sizeof(timebuffer), gps_time_ns()));
    }


    // Basically loop until we're supposed stop
    while(!stop_processing) {

        // -----------------------------------
        // start:  resolve and connect to aprs-is server
        //
        // Resolve and connect to the APRS network server
        
        struct addrinfo hints;
        memset(&hints,0,sizeof(hints));
        hints.ai_family = PF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        hints.ai_flags = AI_CANONNAME|AI_ADDRCONFIG;

        struct addrinfo *results = NULL;
        int ecode;

        // Try a few times in case we come up before the resolver is quite ready
        for(int tries=0; tries < 10; tries++) {
            if((ecode = getaddrinfo(Host,Port,&hints,&results)) == 0)
                break;
            fprintf(Logfile, "%s resolver loop sleeping for 500ms.\n", format_gpstime(timebuffer,sizeof(timebuffer), gps_time_ns()));
            struct timespec tv;
            tv.tv_sec = 0;
            tv.tv_nsec = 5 * 100000000ll;
            //usleep(500000); // 500 ms
            nanosleep(&tv, NULL);
        }

        if(ecode != 0) {
            fprintf(stderr,"Can't getaddrinfo(%s,%s): %s\n",Host,Port,gai_strerror(ecode));
            fprintf(Logfile, "%s ecode sleeping for 5s.\n", format_gpstime(timebuffer,sizeof(timebuffer), gps_time_ns()));

            struct timespec tv;
            tv.tv_sec = 5;
            tv.tv_nsec = 0;
            nanosleep(&tv, NULL);
            //usleep(5000000); // 5 sec
            continue; // Keep trying
        }

        struct addrinfo *resp;
        for(resp = results; resp != NULL; resp = resp->ai_next) {
            if((Network_fd = socket(resp->ai_family,resp->ai_socktype,resp->ai_protocol)) < 0)
                continue;
            if(connect(Network_fd,resp->ai_addr,resp->ai_addrlen) == 0)
                break;
            close(Network_fd);
            Network_fd = -1;
        }

        if(resp == NULL) {
            fprintf(stderr,"Can't connect to server %s:%s\n",Host,Port);
            fprintf(Logfile, "%s Can't connect to server %s:%s, sleeping for 2mins.\n", format_gpstime(timebuffer,sizeof(timebuffer), gps_time_ns()), Host, Port);

            struct timespec tv;
            tv.tv_sec = 60*2;
            tv.tv_nsec = 0;
            nanosleep(&tv, NULL);
            //sleep(600); // 5 minutes
            freeaddrinfo(results);
            resp = results = NULL;
            continue;
        }

        // end:  resolve and connect to aprs-is server
        // --------------------------------------

        if(Logfile) {
            format_gpstime(timebuffer, sizeof(timebuffer), gps_time_ns());
            fprintf(Logfile,"%s Connected to APRS server %s port %s\n", timebuffer, resp->ai_canonname,Port);
        }

        freeaddrinfo(results);
        resp = results = NULL;

        FILE *network = fdopen(Network_fd,"w+");
        setlinebuf(network);

        // start threads
        pthread_create(&Read_thread,NULL,netreader,NULL);
        if (beaconing_enabled)
            pthread_create(&Beacon_thread,NULL,beaconthread,NULL);
        if (gpsd_enabled)
            pthread_create(&GPSD_thread,NULL,gpsthread,NULL);


        // Log into the APRS-IS server 
        if(fprintf(network,"user %s pass %s vers KA9Q-aprs 1.0\r\n",User,Passcode) <= 0) {

            // if there was error, then we close our connection, sleep for 5mins, then restart this outer 'while loop' back at the beginning.
            fclose(network);
            network = NULL;

            fprintf(Logfile, "%s login step.  sleeping for 5mins.\n", format_gpstime(timebuffer,sizeof(timebuffer), gps_time_ns()));
            struct timespec tv;
            tv.tv_sec = 600;
            tv.tv_nsec = 0;
            nanosleep(&tv, NULL);
            //sleep(600); // 5 minutes;
            continue;
        }


        // close this file descriptor just in case it was open from a prior loop iteration
        //if (Input_fd != -1 && Input_fd != NULL) {
        //    fclose(Input_fd);
        //}

        // Set up multicast input
        {
            struct sockaddr_storage sock;
            resolve_mcast(Mcast_address_text,&sock,DEFAULT_RTP_PORT,NULL,0);
            Input_fd = listen_mcast(&sock,NULL);
        }
        if (Input_fd == -1) {
            fprintf(stderr,"Can't set up multicast input from %s\n",Mcast_address_text);
            exit(EX_IOERR);
        }

        uint8_t packet[PKTSIZE];
        int size;

        // loop until we've been signaled to stop processing
        while(!stop_processing) {

            struct pollfd pollfd[1];
            pollfd[0].fd = Input_fd;
            pollfd[0].events = POLLIN;

            // wait for 50ms for new data to appear on the input socket (i.e. the multicast RTP stream)
            poll(pollfd, 1, 50);

            // if there was data available then read it it.
            if(pollfd[0].revents & POLLIN) {
                // there *should* be data available on the socket, soooo, this shouldn't block.
                size = recv(Input_fd, packet, sizeof(packet), 0);
            }
            else {
                // no data available on the socket, go back to the start of the loop
                continue;
            }


            struct rtp_header rtp_header;
            uint8_t const *dp = packet;

            dp = ntoh_rtp(&rtp_header,dp);
            size -= dp - packet;

            if(rtp_header.pad) {
                // Remove padding
                size -= dp[size-1];
                rtp_header.pad = 0;
            }

            if(size <= 0)
                continue;  // Bogus RTP header?

            if(rtp_header.type != AX25_pt)
                continue; // Wrong type

            // print the start of a log message.  this includes timestamp, etc..but no newline...as that'll be written down below...
            if(Logfile) {
                // Emit local timestamp
                char result[1024];

                fprintf(Logfile,"%s ssrc %u seq %d",
                        format_gpstime(result,sizeof(result),gps_time_ns()),
                        rtp_header.ssrc,rtp_header.seq);
            }

            // Parse incoming AX.25 frame
            struct ax25_frame frame;
            if(ax25_parse(&frame,dp,size) < 0) {
                if(Logfile)
                    fprintf(Logfile," Unparsable packet\n");

                pthread_mutex_lock(&stats_lock);
                dropped_pkts++;
                pthread_mutex_unlock(&stats_lock);

                // restart at the beginning of this inner loop as this was an unparsable AX25 frame
                continue;
            }

            // Construct TNC2-style monitor string for APRS reporting
            char monstring[2048]; // Should be large enough for any legal AX.25 frame; we'll assert this periodically
            int sspace = sizeof(monstring);
            int infolen = 0;
            int is_tcpip = 0;


            // ------------------- start:  process the incoming packet ---------------
            {
                memset(monstring,0,sizeof(monstring));
                char *cp = monstring;
                {
                    int w = snprintf(cp,sspace,"%s>%s",frame.source,frame.dest);
                    cp += w;
                    sspace -= w;
                    assert(sspace > 0);
                }
                for(int i=0; i<frame.ndigi; i++) {
                    // if "TCPIP" appears, this frame came off the Internet and should not be sent back to it
                    if(strcmp(frame.digipeaters[i].name,"TCPIP") == 0)
                        is_tcpip = 1;
                    int const w = snprintf(cp,sspace,",%s%s",frame.digipeaters[i].name,frame.digipeaters[i].h ? "*" : "");
                    cp += w;
                    sspace -= w;
                    assert(sspace > 0);
                }
                {
                    // qAR means a bidirectional i-gate, qAO means receive-only
                    //    w = snprintf(cp,sspace,",qAR,%s",User);
                    int const w = snprintf(cp,sspace,",qAO,%s",User);
                    cp += w;
                    sspace -= w;
                    *cp++ = ':';
                    sspace--;
                    assert(sspace > 0);
                }
                for(int i=0; i < frame.info_len; i++) {
                    char const c = frame.information[i] & 0x7f; // Strip parity in monitor strings
                    if(c != '\r' && c != '\n' && c != '\0') {
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
            // ------------------- end:  process the incoming packet ---------------


            // make sure there wasn't something odd happening with the buffers
            assert(sizeof(monstring) - sspace - 1 == strlen(monstring));

            // print the rest of the log message
            if(Logfile)
                fprintf(Logfile," %s\n",monstring);

            // ------------------- start:  check for drop conditions ---------------
            if(frame.control != 0x03 || frame.type != 0xf0) {
                if(Logfile)
                    fprintf(Logfile," Not relaying: invalid ax25 ctl/protocol\n");

                pthread_mutex_lock(&stats_lock);
                dropped_pkts++;
                pthread_mutex_unlock(&stats_lock);

                continue;
            }
            if(infolen == 0) {
                if(Logfile)
                    fprintf(Logfile," Not relaying: empty I field\n");

                pthread_mutex_lock(&stats_lock);
                dropped_pkts++;
                pthread_mutex_unlock(&stats_lock);

                continue;
            }
            if(is_tcpip) {
                if(Logfile)
                    fprintf(Logfile," Not relaying: Internet relayed packet\n");

                pthread_mutex_lock(&stats_lock);
                dropped_pkts++;
                pthread_mutex_unlock(&stats_lock);

                continue;
            }
            if(frame.information[0] == '{') {
                if(Logfile)
                    fprintf(Logfile," Not relaying: third party traffic\n");

                pthread_mutex_lock(&stats_lock);
                dropped_pkts++;
                pthread_mutex_unlock(&stats_lock);

                continue;
            }
            // ------------------- end:  check for drop conditions ---------------

            pthread_mutex_lock(&stats_lock);
            received_pkts++;
            if (rtp_header.ssrc == 145825)
                received_sat_pkts++;
            pthread_mutex_unlock(&stats_lock);

            int ret;

            // Send to APRS network with appended crlf
            pthread_mutex_lock(&tcp_lock);
            ret = fprintf(network,"%s\r\n",monstring);
            pthread_mutex_unlock(&tcp_lock);

            if (ret <= 0) {

                fprintf(stderr,"Error communicating to server, %s:%s\n",Host,Port);
                fprintf(Logfile, "%s Exiting, error communicating to server, %s:%s.\n", format_gpstime(timebuffer,sizeof(timebuffer), gps_time_ns()), Host, Port);

                // error!
                fclose(network);
                network = NULL;
                //goto retry; // Try to reopen the network connection
                            
                // close the threads
                pthread_cancel(Read_thread);
                pthread_join(Read_thread,NULL);
                pthread_cancel(Beacon_thread);
                pthread_join(Beacon_thread,NULL);
                pthread_cancel(GPSD_thread);
                pthread_join(GPSD_thread,NULL);

                // break out of this inner loop
                break;
            }

        } // inner while loop
    } // outer while loop

    if (Logfile) 
        fprintf(Logfile, "%s Done.\n", format_gpstime(timebuffer, sizeof(timebuffer), gps_time_ns()));

    if (Logfilename && Logfile)
        fclose(Logfile);
}


// ---------------------------------------------------------------------
// ---------------------------------------------------------------------
// Just read and echo responses from the APRS-IS server
void *netreader(void *arg)
{
    pthread_setname("aprs-read");

    char timebuffer[1024];
    char *line = NULL;
    size_t linecap = 0;
    ssize_t linelen;
    int num = 0;
    bool beacon_locked = true;

    // Create our own stream; there seem to be problems sharing a common stream among threads
    FILE *network = fdopen(Network_fd,"r");

    // Loop continuously as long as we're getting data from the APRS-IS servers
    while((linelen = getline(&line,&linecap,network)) > 0) {

        // increment our counter
        num++;

        // write all incoming data from the APRS-IS server to our log file
        if(Logfile) {

            // right trim off any newline/carrage return/spaces from the line we read from the APRS-IS server and place '\0' char at that location.
            char *end = line + strlen(line) -1;
            while (end > line && isspace(*end)) {
                end--;
            }
            *(end+1) = '\0';

            fprintf(Logfile, "%s %s: %s\n", format_gpstime(timebuffer, sizeof(timebuffer), gps_time_ns()), Host, line);
            //fwrite(line,linelen,1,Logfile);
        }

        // if more 2 lines have been received from the APRS-IS server, then we signal the writing thread that its clear to xmit
        if (num > 1 && beacon_locked) {
            pthread_cond_signal(&cleartoxmit);
            beacon_locked = false;
        }
    }

    FREE(line);
    return NULL;
}


// ---------------------------------------------------------------------
// ---------------------------------------------------------------------
// Initialize station structure
void initstation(APRS_STATION *station)
{
    station->lat = 0;
    station->lon = 0;
    station->alt = 0;
    station->course = 0;
    station->speed = 0;
    memset(station->symbol, 0, sizeof(station->symbol));
    memset(station->overlay, 0, sizeof(station->overlay));
    memset(station->comment, 0, sizeof(station->comment));
}


// ---------------------------------------------------------------------
// ---------------------------------------------------------------------
// Shutdown the application.  Should be called from kill signals.
void closedown(int x)
{
    char timebuffer[1024];

    if (Verbose)
        fprintf(Logfile, "%s Stopping app.  Received signal: %d\n", format_gpstime(timebuffer,sizeof(timebuffer), gps_time_ns()), x);

    // Set the global to true so that processing loops close down
    stop_processing = true;

}


// ---------------------------------------------------------------------
// ---------------------------------------------------------------------
// read the telemetry sequence from the telemetry sequence file
int readtelemseq(char *filename)
{
    FILE *telemfile;
    int i = 0;

    // Read in the last sequence from the telemetry file
    if (filename) {
        telemfile = fopen(filename, "r");

        char *line = NULL;
        size_t linecap = 0;
        ssize_t linelen;

        // read in the first line from the telemetry file
        if (telemfile) {
            linelen = getline(&line, &linecap, telemfile);

            // if we got something back, then we convert that to an integer and use that value as our starting sequence number.
            if (linelen > 0) {
                i = atoi(line);

                // check for rollover or oddness
                if (i > 999 || i < 0)
                    i = 0;
            }

            // close the file (ending sequence number is written to the telemfile upon application end)
            fclose(telemfile);
        }
        FREE(line);
    }

    return i;
}

// ---------------------------------------------------------------------
// ---------------------------------------------------------------------
// write the telemetry sequence to the telemetry sequence file
void writetelemseq(char *filename, int seq)
{
    FILE *telemfile;

    // write the sequence provide to the file.  This will overwrite the file, btw.
    if (filename) {
        telemfile = fopen(filename, "w");
        fprintf(telemfile, "%d\n", seq);
        fclose(telemfile);
    }
}

// ---------------------------------------------------------------------
// ---------------------------------------------------------------------
// Construct a packet string for beaconing our position, symbol, and comment.
int positpacket(char *buffer, size_t n, char *info)
{
    // sanity check
    if (buffer == NULL)
        return 0;

    time_t t = time(NULL);
    struct tm *ts = gmtime(&t);
    int hours = ts->tm_hour;
    int minutes = ts->tm_min;
    int seconds = ts->tm_sec;

    return snprintf (buffer, n, "%s>%s,TCPIP*:/%02d%02d%02dh%s", User, tocall, hours, minutes, seconds, info);

}


// ---------------------------------------------------------------------
// ---------------------------------------------------------------------
// construct the telemetry report packet
int telemetrypacket(char *buffer, size_t n, APRS_EQNS *eqns)
{
    int num = 0;
    int received;
    int dropped;
    int received_sat;
    int adjusted_receive;
    int adjusted_dropped;
    int adjusted_receive_sat;

    // sanity check
    if (buffer == NULL)
        return 0;

    // lock on the stats mutex
    pthread_mutex_lock(&stats_lock);

    // save the received and dropped packet values to local variables, then reset them back to zero
    received = received_pkts;
    dropped = dropped_pkts;
    received_sat = received_sat_pkts;
    received_pkts = 0;
    dropped_pkts = 0;
    received_sat_pkts = 0;


    // unlock the stats mutex
    pthread_mutex_unlock(&stats_lock);

    // Determine coefficients for the APRS equations packet.  We do this because telemetry values can only range from 0-255.
    adjusted_receive = calculatecoef(received, &(eqns->rx));
    adjusted_dropped = calculatecoef(dropped, &(eqns->drp));
    adjusted_receive_sat = calculatecoef(received_sat, &(eqns->rxsat));

    // write the telemetry packet string to the supplied buffer.
    num = snprintf(buffer, n, "%s>%s,TCPIP*:T#%03d,%03d,%03d,%03d,%03d,%03d,00000000,Telemetry report", User, tocall, sequence, adjusted_receive, adjusted_dropped, adjusted_receive_sat, 0, 0);

    // check the sequence number.  If > 999, then we roll it back to 000.
    sequence++;
    if (sequence > 999 || sequence < 0)
        sequence = 0;

    // write the last telemetry sequence to the telemetry sequence file
    writetelemseq(TelemSeqFilename, sequence);

    return num;

}


// ---------------------------------------------------------------------
// ---------------------------------------------------------------------
// for a given set of station details, fill a buffer with the constructed information string (as part of an APRS packet)
int createinfostring(char *buffer, size_t n, APRS_STATION *station)
{
    float local_lat, local_lon, lat_ms, lon_ms;
    int lat_d, lon_d, infosize, num;
    char lat_ns;
    char lon_ew;
    char lat_string[20];
    char lon_string[20];
    char csespd[8]; // only if this station mobile (ex. ismobile = true)
                    
    // convert the lat/lon to degrees, decimal minutes
    if (station->lat >= 0)
        lat_ns = 'N';
    else
        lat_ns = 'S';

    if (station->lon >= 0)
        lon_ew = 'E';
    else
        lon_ew = 'W';

    // remove any negative degrees
    local_lat = (station->lat < 0 ? -station->lat : station->lat);
    local_lon = (station->lon < 0 ? -station->lon : station->lon);

    // just the degrees (truncate off the decimal portion)
    lat_d = (int) local_lat;
    lon_d = (int) local_lon;

    // the decimal minutes
    lat_ms = (local_lat - lat_d) * 60;
    lon_ms = (local_lon - lon_d) * 60;

    // the latitude and longitude strings
    memset(lat_string, 0, sizeof(lat_string));
    memset(lon_string, 0, sizeof(lon_string));

    // For APRS, the position report represents latitude as ddmm.ssN or ddmm.ssS
    // For APRS, the position report represents longitude as dddmm.ssWor dddmm.ssE
    snprintf(lat_string, sizeof(lat_string), "%02d%05.2f%c", lat_d, lat_ms, lat_ns);
    snprintf(lon_string, sizeof(lon_string), "%03d%05.2f%c", lon_d, lon_ms, lon_ew);


    // construct the course/speed string
    memset(csespd, 0, sizeof(csespd));
    if (ismobile) {
        double c = round(station->course);
        double s = round(station->speed * 10);

        // only if this station is moving at > 1 MPH
        if (s > 0) {
            snprintf (csespd, sizeof(csespd), "%03.0f/%03.0f", c, s);
        }
        else 
            snprintf (csespd, sizeof(csespd), "000/000");
    }

    // Determine if the buffer is shorter than the maximum size for a position report packet
    if (n > MAX_POSIT)
        infosize = MAX_POSIT;
    else
        infosize = n;


    // handle the symbol/overlay BS'ery with APRS
    char sym[2];
    char ovl[2];

    // clear the buffers
    sym[0] = sym[1] = ovl[0] = ovl[1] = '\0';

    // is the first character of the symbol the primary or alternate table identifier?
    if (strncmp(station->symbol, "/", 1) == 0) {
        // primary table selected
        sym[0] = station->symbol[1];
        ovl[0] = '/';
    }
    else if (strncmp(station->symbol, "\\", 1) == 0) {
        // alternate table selected
        sym[0] = station->symbol[1];
        if (station->overlay[0] != '\0')
            ovl[0] = station->overlay[0];
        else
            ovl[0] = '\\';
    }
    else {
        // just default to a red dot
        sym[0] = '/';
        ovl[0] = '/';
    }
    
    // Create the information string, saved to the buffer.  If the comment string is too long, then that is truncated at the 'infosize' mark.
    num = snprintf(buffer, infosize, "%s%s%s%s%s/A=%06d%s", lat_string, ovl, lon_string, sym, csespd, (int) station->alt, station->comment);

    // return the number of bytes written to the buffer
    return num;
}



// ---------------------------------------------------------------------
// ---------------------------------------------------------------------
// calculate the APRS coefficients for the equations packet of the "value" parameter
int calculatecoef(int value, APRS_COEF *c) 
{
    int adjusted_value = value;

    if (value <= 255) {
        c->a = 0;
        c->b = 1;
        c->c = 0;
    }
    else {
        c->a = 0;
        c->b = value / 255;
        c->c = value % 255;
        adjusted_value = 255;
    }

    return adjusted_value;
}

// ---------------------------------------------------------------------
// ---------------------------------------------------------------------
// construct the APRS equation coefficents packet
int eqnpacket(char *buffer, size_t n, APRS_EQNS *eqns)
{
    if (buffer == NULL)
        return 0;

    return snprintf(buffer, n, "%s>%s,TCPIP*::%-9s:EQNS.%d,%d,%d,%d,%d,%d,%d,%d,%d,0,0,0,0,0,0", 
            User, tocall, User, eqns->rx.a, eqns->rx.b, eqns->rx.c, eqns->drp.a, eqns->drp.b, eqns->drp.c, eqns->rxsat.a, eqns->rxsat.b, eqns->rxsat.c);

}

// ---------------------------------------------------------------------
// ---------------------------------------------------------------------
// construct the APRS parameter name packet
int parampacket(char *buffer, size_t n)
{
    if (buffer == NULL)
        return 0;

    return snprintf(buffer, n, "%s>%s,TCPIP*::%-9s:PARM.Rx10m,Drop10m,RxSat10m", User, tocall, User);

}

// ---------------------------------------------------------------------
// ---------------------------------------------------------------------
// construct the APRS units packet
int unitspacket(char *buffer, size_t n)
{
    if (buffer == NULL)
        return 0;

    return snprintf(buffer, n, "%s>%s,TCPIP*::%-9s:UNIT.Pkts,Pkts,Pkts", User, tocall, User);

}

// ---------------------------------------------------------------------
// ---------------------------------------------------------------------
// construct the APRS bitsense packet
int bitspacket(char *buffer, size_t n)
{
    if (buffer == NULL)
        return 0;

    return snprintf(buffer, n, "%s>%s,TCPIP*::%-9s:BITS.00000000,KA9Q-radio Telemetry", User, tocall, User);

}

// ---------------------------------------------------------------------
// ---------------------------------------------------------------------
// send a position packet to the APRS-IS server
void *beaconthread(void *arg)
{
    pthread_setname("beacon-thread");

    // Buffer where we save the position packet that we "might" beacon to the APRS-IS server.
    char packet_string[332]; // the maximum size of an AX.25 UI frame is 332 bytes.
    char info_string[256];  // max size of the APRS information field
    int ret = 1;

    // The maximum amount of time (in seconds) we wait for before sending position packets to the APRS-IS server.
    int posit_max = 120;

    // we only want to send a set of telemetry packets every 10mins or so.  
    int telemetry_max = 600;

    // last xmit counters
    int elapsed_posit = posit_max - 20;
    int elapsed_telemetry = 0;

    // xmit flags
    bool xmit_telemetry = false;
    bool xmit_posit = false;

    // In the case of GPSD, we want to track the position of this station the last time we transmitted
    double last_lat = 0;
    double last_lon = 0;
    double last_alt = 0;

    // buffers 
    char timestring[1024];
    char eqns[256];
    char params[256];
    char units[256];
    char bits[256];
    char telem[256];
    APRS_EQNS aprs_eqns;

    // Create our own stream; there seem to be problems sharing a common stream among threads
    FILE *network = fdopen(Network_fd,"w");
    setlinebuf(network);


    // We don't want to conenct to the APRS-IS server until our connection to that server was successful.  So we wait until the reader thread has received confirmation from the APRS-IS server and has 
    // signaled that we can continue.
    
    // Attempt to get the beacon_lock that signifies that we can proceed with transmitting our beacons to the APRS-IS servers
    pthread_mutex_lock(&beacon_lock);

    // ..and now wait for the signal from the reader thread on if we can proceed or not...
    pthread_cond_wait(&cleartoxmit, &beacon_lock);

    // unlock as we're now clear to xmit to the APRS-IS server
    pthread_mutex_unlock(&beacon_lock);

    // Loop until we can't send data (i.e. something went wrong when talking to the APRS-IS server) or we've been told to stop
    while(ret > 0 && !stop_processing) {

        // set the transmit flags
        xmit_telemetry = false;
        xmit_posit = false;
        
        if (gpsd_enabled) {

            // lock the aprs_station structure to determine if our position has changed
            pthread_mutex_lock(&aprs_station_lock);
            if (gpsmode >= 3 && (((aprs_station.lat - last_lat > 0.0001 || aprs_station.lon - last_lon > 0.0001 || aprs_station.alt - last_alt > 100) && elapsed_posit > posit_max / 4) || elapsed_posit > posit_max)) {
                last_lat = aprs_station.lat;
                last_lon = aprs_station.lon;
                last_alt = aprs_station.alt;

                // initialize our info_string buffer
                memset(info_string, 0, sizeof(info_string));

                // create the information field for this position packet
                if (createinfostring(info_string, sizeof(info_string), &aprs_station) > 0) {

                    // initialize out packet buffer
                    memset(packet_string, 0, sizeof(packet_string));

                    // create a new position packet that we can send to the APRS-IS server
                    positpacket(packet_string, sizeof(packet_string), info_string);

                    // set the xmit flag for position packets.
                    xmit_posit = true;
                }
            }

            // unlock 
            pthread_mutex_unlock(&aprs_station_lock);

        }
        else {  // not GPSD enabled, so we'll just look to periodically transmit the static position of this station

            // has it been long enough?
            if (elapsed_posit > posit_max) {

                // initialize our info_string buffer
                memset(info_string, 0, sizeof(info_string));

                // create the information field for this position packet
                if (createinfostring(info_string, sizeof(info_string), &aprs_station) > 0) {

                    // initialize out packet buffer
                    memset(packet_string, 0, sizeof(packet_string));

                    // create a new position packet that we can send to the APRS-IS server
                    positpacket(packet_string, sizeof(packet_string), info_string);

                    // set the xmit flag for position packets.
                    xmit_posit = true;
                }
            }
        }

        // has it been long enough?  ..and it's now time to send a set of telemetry packets the APRS-IS server
        // For telemetry packets it's simple as we just need to wait a specific amount of time before sending.
        if (elapsed_telemetry > telemetry_max) {

            // construct telemetry packets
            telemetrypacket(telem, sizeof(telem), &aprs_eqns);
            eqnpacket(eqns, sizeof(eqns), &aprs_eqns);
            parampacket(params, sizeof(params));
            unitspacket(units, sizeof(units));
            bitspacket(bits, sizeof(bits));

            xmit_telemetry = true;
        }

        // lock the connection to the APRS-IS server.
        if (xmit_posit || xmit_telemetry)
            pthread_mutex_lock(&tcp_lock);


        if (xmit_posit) {

            // Send our position packet to the APRS-IS server
            fprintf(Logfile, "%s xmitting packet: %s\n", format_gpstime(timestring,sizeof(timestring), gps_time_ns()), packet_string);
            ret = fprintf(network, "%s\r\n", packet_string);

            // reset elapsed time since last transmission.
            elapsed_posit = 0;
        }

        if (ret && xmit_telemetry) {

            fprintf(Logfile, "%s xmitting packet: %s\n", format_gpstime(timestring,sizeof(timestring), gps_time_ns()), telem);
            fprintf(Logfile, "%s xmitting packet: %s\n", format_gpstime(timestring,sizeof(timestring), gps_time_ns()), eqns);
            fprintf(Logfile, "%s xmitting packet: %s\n", format_gpstime(timestring,sizeof(timestring), gps_time_ns()), params);
            fprintf(Logfile, "%s xmitting packet: %s\n", format_gpstime(timestring,sizeof(timestring), gps_time_ns()), units);
            fprintf(Logfile, "%s xmitting packet: %s\n", format_gpstime(timestring,sizeof(timestring), gps_time_ns()), bits);

            // send telemetry packets to the APRS-IS server
            if (ret)
                ret = fprintf(network, "%s\r\n", telem);
            if (ret)
                ret = fprintf(network, "%s\r\n", eqns);
            if (ret)
                ret = fprintf(network, "%s\r\n", params);
            if (ret)
                ret = fprintf(network, "%s\r\n", units);
            if (ret)
                ret = fprintf(network, "%s\r\n", bits);

            // reset the telemetry elapsed counter
            elapsed_telemetry = 0;
        }

        // unlock the connection to the APRS-IS server.
        if (xmit_posit || xmit_telemetry)
            pthread_mutex_unlock(&tcp_lock);

        // sleep for some small number of seconds
        if (ret) {
            struct timespec tv;
            tv.tv_sec = 10;  // 10 seconds
            tv.tv_nsec = 0;
            nanosleep(&tv, NULL);

            // increment counters
            elapsed_posit += 10;
            elapsed_telemetry += 10;
        }
        else 
            fprintf(stderr, "Error sending packet(s) to the APRS-IS server.\n");
    }

    return NULL;
}


// the GPS thread
void *gpsthread(void *arg)
{
    pthread_setname("gps-thread");

    // buffer to save timeformatted output
    char timebuffer[1024];

    // we'll store GPS data in this structure.
    struct gps_data_t gps_data;

    // number of attempts to connect to GPSD
    int trycount = 0;

    // GPSD Host possibilities
    char *gpshost = "localhost";

    // outer loop for (re)connecting to the GPSD instance
    while (!stop_processing) {

        if (GPSDHost != NULL)
            gpshost = GPSDHost;

        if (0 != gps_open(gpshost, "2947", &gps_data)) {
            fprintf(Logfile, "%s Unable to connect to GPSD on %s.  trycount=%d\n", format_gpstime(timebuffer, sizeof(timebuffer), gps_time_ns()), gpshost, trycount);

            // sleep for a few microseconds.  Ramping up the sleep time as the trycount gets large (i.e. we've lost connectivity all together...no sense in just continually retrying).
            float usecs = 10 * exp(trycount);
            usleep(usecs);
        }
        else {
            fprintf(Logfile, "%s Connected to GPSD on %s.\n", format_gpstime(timebuffer, sizeof(timebuffer), gps_time_ns()), gpshost);
        }


        (void) gps_stream(&gps_data, WATCH_ENABLE | WATCH_JSON, NULL);
        
        double change_threshold = .0001;
        double prev_lat, prev_lon;
        prev_lat = prev_lon = 0;

        // Loop continuously reading from GPSD
        while (gps_waiting(&gps_data, 5000000) && !stop_processing) {
            if (-1 == gps_read(&gps_data, NULL, 0)) {
                fprintf(Logfile, "%s Unable to read from GPSD on %s.\n", format_gpstime(timebuffer, sizeof(timebuffer), gps_time_ns()), gpshost);
                break;
            }

            // check that a GPS mode has been set.  If not, then just abort and restart at the top of this inner loop.
            if (MODE_SET != (MODE_SET & gps_data.set)) {
                continue;
            }

            // If the lat/lon is valid, then check if we should update the GPS location variables
            if (isfinite(gps_data.fix.latitude) && isfinite(gps_data.fix.longitude)) {

                // only update the our GPS position variables if our position has changed by > 0.0001 degrees, and we have a 3D fix.
                if ((fabs(gps_data.fix.latitude - prev_lat) > change_threshold || fabs(gps_data.fix.longitude - prev_lon) > change_threshold) && gps_data.fix.mode >= 3) {

                    if (Verbose)
                        fprintf(stdout, "GPS position change: %d %.6f, %.6f\n", gps_data.fix.mode, gps_data.fix.latitude, gps_data.fix.longitude);


                    // Update the GPS lat/lon variables on the global aprs_station structure.
                    pthread_mutex_lock(&aprs_station_lock);
                    gpsmode = gps_data.fix.mode;
                    aprs_station.lat = gps_data.fix.latitude;
                    aprs_station.lon = gps_data.fix.longitude;
                    aprs_station.alt = gps_data.fix.altitude * 3.2808399; // converted to feet
                    aprs_station.speed = gps_data.fix.speed * 2.236936; // converted to mph
                    aprs_station.course = gps_data.fix.track;
                    pthread_mutex_unlock(&aprs_station_lock);

                }
                
                // reset the trycount since we've just read data from the GPS
                if (trycount > 0) {
                    fprintf(Logfile, "%s Reconnected to GPSD on %s.  GPS mode: %d.\n", format_gpstime(timebuffer, sizeof(timebuffer), gps_time_ns()), gpshost, gps_data.fix.mode);
                    trycount = 0;
                }

                prev_lat = gps_data.fix.latitude;
                prev_lon = gps_data.fix.longitude;

            } 
        }
    }

    // When you are done...
    (void)gps_stream(&gps_data, WATCH_DISABLE, NULL);
    (void)gps_close(&gps_data);
    fprintf(Logfile, "%s GPSD thread ended.\n", format_gpstime(timebuffer, sizeof(timebuffer), gps_time_ns()));

    return NULL;
}
